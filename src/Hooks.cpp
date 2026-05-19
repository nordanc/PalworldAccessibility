#include "Hooks.hpp"
#include "Speech.hpp"
#include "Hotkeys.hpp"

#include <DynamicOutput/Output.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/String.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

using namespace RC;

namespace PalAccess {

namespace {

template <class T>
T* StackLocalsAs(Unreal::FFrame& Stack) {
    return reinterpret_cast<T*>(Stack.Locals());
}

bool ClassNameStartsWith(Unreal::UObject* Context, std::wstring_view prefix) {
    if (!Context) return false;
    auto cls = Context->GetClassPrivate();
    if (!cls) return false;
    return cls->GetName().starts_with(prefix);
}

bool ClassNameIs(Unreal::UObject* Context, std::wstring_view name) {
    if (!Context) return false;
    auto cls = Context->GetClassPrivate();
    if (!cls) return false;
    return cls->GetName() == name;
}

bool FunctionNameIs(Unreal::FFrame& Stack, std::wstring_view name) {
    auto node = Stack.Node();
    if (!node) return false;
    return node->GetName() == name;
}

std::wstring ClassNameOf(Unreal::UObject* obj) {
    if (!obj) return L"<null>";
    auto cls = obj->GetClassPrivate();
    return cls ? cls->GetName() : std::wstring(L"<no-class>");
}

// UObject instance name (e.g. "StartMultiGame", "Settings_2").
std::wstring InstanceNameOf(Unreal::UObject* obj) {
    return obj ? obj->GetName() : std::wstring();
}

// Walk the outer chain looking for an ancestor of a specific class. Skips
// WidgetTree etc. transparently because they're not the target class.
Unreal::UObject* FindAncestorOfClass(Unreal::UObject* obj,
                                     std::wstring_view class_name,
                                     int max_depth = 10) {
    Unreal::UObject* cur = obj;
    for (int d = 0; cur && d < max_depth; ++d) {
        if (ClassNameOf(cur) == class_name) return cur;
        cur = cur->GetOuterPrivate();
    }
    return nullptr;
}

// Strip a trailing "_<digits>" suffix from an instance name. UE auto-appends
// numeric suffixes ("StartGame_2") to disambiguate duplicates; we want the
// semantic root.
std::wstring StripNumericSuffix(std::wstring s) {
    auto last = s.rfind(L'_');
    if (last == std::wstring::npos || last + 1 >= s.size()) return s;
    for (size_t i = last + 1; i < s.size(); ++i) {
        if (s[i] < L'0' || s[i] > L'9') return s;
    }
    s.resize(last);
    return s;
}

std::wstring StripPrefix(std::wstring s, std::wstring_view prefix) {
    if (s.starts_with(prefix)) s.erase(0, prefix.size());
    return s;
}

// Turn a CamelCase / underscore-separated identifier into a human-readable
// phrase. Strips well-known wrapper noise so e.g.
//   WBP_InGameMenu_Construction_Icon_C -> "Construction"
//   WBP_Graphic_Settings_C             -> "Graphic Settings"
//   StartMultiGame_JoinServer          -> "Start Multi Game Join Server"
std::wstring Humanize(std::wstring_view in_view) {
    std::wstring in(in_view);

    // Strip class-name prefixes ("WBP_", etc.).
    static constexpr std::wstring_view kClassPrefixes[] = {
        L"WBP_", L"UMG_", L"UI_", L"W_", L"BP_"
    };
    for (auto p : kClassPrefixes) {
        if (in.starts_with(p)) { in.erase(0, p.size()); break; }
    }
    // Strip trailing "_C".
    if (in.ends_with(L"_C")) in.resize(in.size() - 2);

    // Strip wrapper-decoration suffixes ("_Icon", "_Button", "_Widget"…).
    static constexpr std::wstring_view kWrapperSuffixes[] = {
        L"_Icon", L"_Button", L"_btn", L"_Widget", L"_Item",
        L"_Image", L"_Text", L"_Label", L"_Slot"
    };
    for (auto s : kWrapperSuffixes) {
        if (in.ends_with(s)) { in.resize(in.size() - s.size()); break; }
    }

    // Strip subsystem prefixes that describe the panel parent, not the
    // semantic element (e.g. "InGameMenu_Construction" → "Construction").
    // Palworld uses both "InGameMenu_" and "IngameMenu_" (lowercase g) at
    // different sites — include every casing.
    static constexpr std::wstring_view kSubsystemPrefixes[] = {
        L"InGameMenu_", L"IngameMenu_", L"Ingame_Menu_",
        L"MainMenu_", L"Menu_",
        L"PalInGameMenu_", L"PalIngameMenu_", L"PalMenu_",
        L"OptionSettings_", L"Settings_", L"Setting_",
        L"Title_"
    };
    for (auto p : kSubsystemPrefixes) {
        if (in.starts_with(p)) { in.erase(0, p.size()); break; }
    }

    // CamelCase + underscore → spaces.
    std::wstring out;
    out.reserve(in.size() + 4);
    for (size_t i = 0; i < in.size(); ++i) {
        wchar_t c = in[i];
        if (c == L'_') {
            if (!out.empty() && out.back() != L' ') out += L' ';
            continue;
        }
        const bool is_upper = (c >= L'A' && c <= L'Z');
        const bool prev_lower = (i > 0 && in[i - 1] >= L'a' && in[i - 1] <= L'z');
        if (i > 0 && is_upper && prev_lower) out += L' ';
        out += c;
    }
    return out;
}

// Hand-curated mapping from semantic UObject-instance tags (the part of a
// button widget's instance name AFTER the class-prefix) to spoken labels.
// Applies to every tagged button: WBP_Title_MenuButton_C, WBP_Title_SettingsButton_C,
// WBP_Title_WorldSelectButton_C, WBP_GuildHeadButton_C, etc. Extend as
// instances are discovered (look for `[PalAccess] tagged button:` log lines).
struct ButtonNameEntry {
    std::wstring_view instance_name;
    std::wstring_view spoken;
};
constexpr ButtonNameEntry kButtonLabels[] = {
    // -- Title menu (confirmed)
    { L"StartLocalGame",            L"Start single player game" },
    { L"StartMultiGame",            L"Multiplayer"              },
    { L"StartMultiGame_JoinServer", L"Join server"              },
    { L"StartMultiGame_InviteCode", L"Join by invite code"      },
    { L"GlobalPalBox",              L"Global Pal box"           },
    { L"Tips",                      L"Tips"                     },
    { L"Option",                    L"Settings"                 },
    { L"News",                      L"News"                     },
    { L"Credit",                    L"Credits"                  },
    { L"ExitGame",                  L"Exit game"                },
    // -- Defensive variants
    { L"StartGame",                 L"Start game"               },
    { L"Settings",                  L"Settings"                 },
    { L"Back",                      L"Back"                     },
    { L"WorldSelect",               L"World select"             },
    { L"HostServer",                L"Host server"              },
    { L"CreateServer",              L"Host server"              },
    // -- Settings categories (confirmed: WBP_OptionSettings_MenuButton tags)
    { L"Graphic",                   L"Graphics"                 },
    { L"Sound",                     L"Audio"                    },
    { L"Game",                      L"Game"                     },
    { L"Control",                   L"Controls"                 },
    { L"Key",                       L"Key bindings"             },
    { L"Other",                     L"Other"                    },
    // Defensive variants
    { L"Graphics",                  L"Graphics settings"        },
    { L"Display",                   L"Display settings"         },
    { L"Audio",                     L"Audio settings"           },
    { L"Controls",                  L"Controls"                 },
    { L"Keyboard",                  L"Keyboard"                 },
    { L"Mouse",                     L"Mouse"                    },
    { L"Gamepad",                   L"Gamepad"                  },
    { L"Language",                  L"Language"                 },
    { L"Accessibility",             L"Accessibility"            },
    // -- World select common actions
    { L"CreateWorld",               L"Create new world"         },
    { L"NewWorld",                  L"Create new world"         },
    { L"DeleteWorld",               L"Delete world"             },
    { L"LoadWorld",                 L"Load world"               },
    { L"Play",                      L"Play"                     },
    { L"PlayWorld",                 L"Play world"               },
    { L"StartGame",                 L"Start game"               },
    { L"NewGame",                   L"New game"                 },
    { L"Continue",                  L"Continue"                 },
    { L"Delete",                    L"Delete"                   },
    { L"Rename",                    L"Rename"                   },
    { L"Copy",                      L"Copy"                     },
    { L"Upload",                    L"Upload to cloud"          },
    { L"Download",                  L"Download from cloud"      },
    { L"CloudSave",                 L"Cloud save"               },
    { L"LocalSave",                 L"Local save"               },
    // -- Multiplayer common actions
    { L"OfficialServer",            L"Official servers"         },
    { L"CommunityServer",           L"Community servers"        },
    { L"DedicatedServer",           L"Dedicated servers"        },
    { L"Refresh",                   L"Refresh"                  },
    { L"Filter",                    L"Filter"                   },
};

std::wstring_view LookupButtonLabel(std::wstring_view tag) {
    for (const auto& e : kButtonLabels) {
        if (tag == e.instance_name) return e.spoken;
    }
    return {};
}

// Walk the outer chain skipping UMG's intermediate WidgetTree containers
// to find the outermost UserWidget — the actual WBP_* blueprint instance.
// `max_depth` caps the climb in case something pathological is going on.
Unreal::UObject* OutermostUserWidget(Unreal::UObject* obj, int max_depth = 8) {
    Unreal::UObject* last_widget = nullptr;
    Unreal::UObject* cur = obj;
    for (int depth = 0; cur && depth < max_depth; ++depth) {
        auto cls_name = ClassNameOf(cur);
        // A UMG UserWidget instance is anything whose class isn't WidgetTree
        // and looks WBP_* / W_* / *Widget*. We just record the deepest such
        // ancestor we find and keep climbing.
        if (cls_name != L"WidgetTree" &&
            (cls_name.starts_with(L"WBP_") ||
             cls_name.starts_with(L"W_")   ||
             cls_name.find(L"Widget") != std::wstring::npos)) {
            last_widget = cur;
        }
        cur = cur->GetOuterPrivate();
    }
    return last_widget;
}

// Friendly-name table for widgets whose label can't be captured from SetText
// (because Palworld bakes it in at design time). Order: longest-prefix wins.
struct FriendlyEntry {
    std::wstring_view class_name;
    std::wstring_view spoken;
};
constexpr FriendlyEntry kFriendly[] = {
    { L"WBP_Title_MenuButton_C",        L"Title menu button"   },
    { L"WBP_Title_SettingsButton_C",    L"Settings"            },
    { L"WBP_Title_WorldSelectButton_C", L"World select button" },
    { L"WBP_Title_WorldMenu_Head_C",    L"World menu"          },
    { L"WBP_Title_WorldSelect_C",       L"World select panel"  },
    { L"WBP_TitleVerText_C",            L"Version info"        },
    { L"WBP_TItle_C",                   L"Title screen"        },
    { L"WBP_TitleMenu_C",               L"Title menu"          },
    { L"WBP_JoinGame_C",                L"Join game panel"     },
    { L"WBP_PalDialog_C",               L"Dialog"              },
    { L"WBP_CommonPopupWindow_C",       L"Pop-up"              },
    { L"WBP_CommonButton_C",            L"Button"              },
    { L"WBP_Menu_btn_C",                L"Menu button"         },
    { L"WBP_PalInvisibleButton_C",      L"Button"              },
    { L"WBP_GuildHeadButton_C",         L"Dropdown"            },
    { L"WBP_PalCommonScrollList_C",     L"Scroll list"         },
};

std::wstring_view FriendlyNameFor(std::wstring_view class_name) {
    for (const auto& e : kFriendly) {
        if (class_name == e.class_name) return e.spoken;
    }
    return {};
}

// Walk the Outer chain looking for the first ancestor that's a UserWidget
// (WBP_*/W_*) AND isn't a button — i.e. the row / panel that CONTAINS the
// hovered button. Used as the target for label extraction.
Unreal::UObject* FindContainingRowWidget(Unreal::UObject* obj, int max_depth = 10) {
    Unreal::UObject* cur = obj;
    for (int d = 0; cur && d < max_depth; ++d) {
        auto cn = ClassNameOf(cur);
        const bool is_widget = (cn.starts_with(L"WBP_") || cn.starts_with(L"W_"));
        const bool is_button = cn.ends_with(L"Button_C") ||
                               cn.ends_with(L"_btn_C")   ||
                               cn == L"WBP_PalInvisibleButton_C";
        if (is_widget && !is_button && cn != L"WidgetTree") return cur;
        cur = cur->GetOuterPrivate();
    }
    return nullptr;
}

// One-shot per-class property dumper. Helps us understand unknown widget
// layouts. Fires the first time DumpPropertiesOnce is called for each
// widget class, then never again for that class.
void DumpPropertiesOnce(Unreal::UObject* widget) {
    if (!widget) return;
    auto cls = widget->GetClassPrivate();
    if (!cls) return;
    auto cn = cls->GetName();

    static std::mutex                       s_mtx;
    static std::unordered_set<std::wstring> s_dumped;
    {
        std::lock_guard<std::mutex> lock(s_mtx);
        if (!s_dumped.insert(cn).second) return;
    }

    Output::send<LogLevel::Verbose>(STR("[PalAccess] ==== props of {} ====\n"), cn);
    int count = 0;
    for (auto* prop : cls->ForEachPropertyInChain()) {
        if (!prop) continue;
        if (++count > 80) break;
        auto pcls = prop->GetClass().GetName();
        auto pname = prop->GetName();
        // For object properties, also log the pointed-to class to spot
        // TextBlock-derived widgets we should be reading.
        std::wstring extra;
        if (pcls == L"ObjectProperty") {
            auto** child_ptr = prop->ContainerPtrToValuePtr<Unreal::UObject*>(widget);
            if (child_ptr && *child_ptr) {
                auto child_cls = (*child_ptr)->GetClassPrivate();
                if (child_cls) extra = L" -> " + child_cls->GetName();
            }
        }
        Output::send<LogLevel::Verbose>(
            STR("[PalAccess]   {} {}{}\n"), pcls, pname, extra);
    }
}

// Safely read the first FText input parameter of the UFunction currently on
// the stack. Walks the function's property chain to find a real FTextProperty
// instead of blind-casting Stack.Locals() to a fabricated struct — that cast
// crashes the game if the function's actual first parameter isn't an FText
// (seen with WBP_PalDialog_C::SetupUI on Confirm of world settings).
std::wstring ReadFirstFTextParam(Unreal::FFrame& Stack) {
    auto* node = Stack.Node();
    if (!node) return {};
    for (auto* prop : node->ForEachProperty()) {
        if (!prop) continue;
        if (prop->GetClass().GetName() != L"TextProperty") continue;
        // Skip the return-value slot, only read input params.
        if ((prop->GetPropertyFlags() & static_cast<uint64_t>(Unreal::EPropertyFlags::CPF_ReturnParm)) != 0) continue;
        auto* locals = Stack.Locals();
        if (!locals) return {};
        auto* ftext = reinterpret_cast<Unreal::FText*>(
            static_cast<uint8_t*>(locals) + prop->GetOffset_Internal());
        return ftext->ToString();
    }
    return {};
}

// Try to read a "Text" FText property off a widget instance. Returns empty
// if the class has no such property.
std::wstring TryReadTextProperty(Unreal::UObject* obj) {
    if (!obj) return {};
    auto c = obj->GetClassPrivate();
    if (!c) return {};
    for (auto* tp : c->ForEachPropertyInChain()) {
        if (!tp) continue;
        if (tp->GetClass().GetName() != L"TextProperty") continue;
        if (tp->GetName() != L"Text") continue;
        auto* ftext = tp->ContainerPtrToValuePtr<Unreal::FText>(obj);
        return ftext ? std::wstring(ftext->ToString()) : std::wstring{};
    }
    return {};
}

// Returns true if a UWidget is currently visible enough to be considered
// part of the focus content. Skips Collapsed (1) and Hidden (2) per
// ESlateVisibility; allows Visible / HitTestInvisible / SelfHitTestInvisible.
bool IsWidgetVisibleForReading(Unreal::UObject* widget) {
    if (!widget) return true;
    auto c = widget->GetClassPrivate();
    if (!c) return true;
    for (auto* prop : c->ForEachPropertyInChain()) {
        if (!prop) continue;
        if (prop->GetName() != L"Visibility") continue;
        auto ptype = prop->GetClass().GetName();
        if (ptype != L"EnumProperty" && ptype != L"ByteProperty") continue;
        auto* val = prop->ContainerPtrToValuePtr<uint8_t>(widget);
        if (!val) continue;
        return (*val != 1 /*Collapsed*/ && *val != 2 /*Hidden*/);
    }
    return true;
}

// Common UE designer placeholder strings we never want to speak.
bool IsPlaceholderText(std::wstring_view s) {
    return s == L"Text" || s == L"text" ||
           s == L"NewTextBlock" ||
           s == L"Lorem ipsum" ||
           s.empty();
}

// Reflection-based label extraction: walk a widget's UProperty chain looking
// for label/value text. Recurses through wrapper UserWidget children (like
// Palworld's BP_PalTextBlock_C which contains a native TextBlock) up to a
// small depth so we get both the label and the value for settings rows.
std::wstring ExtractWidgetLabels(Unreal::UObject* widget, int depth = 0) {
    if (!widget || depth > 2) return {};
    auto cls = widget->GetClassPrivate();
    if (!cls) return {};
    // Dump every class we visit, one time, so future iterations can target
    // state-bearing fields (bool toggles, enum dropdowns, etc.) precisely.
    DumpPropertiesOnce(widget);

    std::wstring result;
    auto append = [&](std::wstring_view s) {
        if (IsPlaceholderText(s)) return;
        if (!result.empty()) result += L", ";
        result.append(s);
    };

    // Pre-pass: scan visible bool properties for toggle state. Used by switch
    // rows (VSync, Motion Blur, etc.) where the state is a bool, not text.
    // We don't append yet — we collect them and emit AFTER the text walk so
    // labels come before state ("VSync, on" not "on, VSync").
    std::wstring pending_state;

    for (auto* prop : cls->ForEachPropertyInChain()) {
        if (!prop) continue;
        auto ptype = prop->GetClass().GetName();

        // FText property on this widget. Read only "Text" (the canonical
        // displayed-text field). Skip ToolTipText / accessibility-summary noise.
        if (ptype == L"TextProperty") {
            auto pname = prop->GetName();
            if (pname == L"ToolTipText") continue;
            if (pname == L"AccessibleSummaryText") continue;
            if (pname == L"Text") {
                auto* ftext = prop->ContainerPtrToValuePtr<Unreal::FText>(widget);
                if (ftext) append(std::wstring_view(ftext->ToString()));
            }
            continue;
        }

        // Toggle/switch state: only on widget classes whose name signals a
        // switch (avoids picking up "Editing" or "bIsActive" on the generic
        // row class). Confirmed field on Palworld's switch: CurrentIsOn.
        if (ptype == L"BoolProperty" && pending_state.empty()) {
            auto class_name_view = cls->GetName();
            const bool is_switch_widget =
                class_name_view.find(L"Switch") != std::wstring::npos ||
                class_name_view.find(L"Toggle") != std::wstring::npos;
            if (!is_switch_widget) continue;
            auto pname = prop->GetName();
            const bool looks_state =
                pname == L"CurrentIsOn" ||
                pname == L"IsOn"        ||
                pname == L"bIsOn"       ||
                pname == L"Toggled"     ||
                pname == L"Switched";
            if (looks_state) {
                auto* val = prop->ContainerPtrToValuePtr<bool>(widget);
                if (val) pending_state = *val ? L"on" : L"off";
            }
            continue;
        }

        if (ptype != L"ObjectProperty") continue;

        // Blacklist property names that hold help / warning / tooltip text.
        // Their parent canvas is typically Hidden when inactive, but the
        // inner TextBlock keeps its "Visible" flag and would be read.
        {
            auto pn = prop->GetName();
            const bool is_noise =
                pn.find(L"Caution")     != std::wstring::npos ||
                pn.find(L"Warning")     != std::wstring::npos ||
                pn.find(L"Tooltip")     != std::wstring::npos ||
                pn.find(L"ToolTip")     != std::wstring::npos ||
                pn.find(L"Hint")        != std::wstring::npos ||
                pn.find(L"Description") != std::wstring::npos ||
                pn.find(L"Help")        != std::wstring::npos ||
                pn.find(L"Error")       != std::wstring::npos ||
                pn.find(L"Notice")      != std::wstring::npos;
            if (is_noise) continue;
        }

        auto** child_ptr = prop->ContainerPtrToValuePtr<Unreal::UObject*>(widget);
        if (!child_ptr || !*child_ptr) continue;
        auto* child = *child_ptr;
        auto child_cls = child->GetClassPrivate();
        if (!child_cls) continue;
        auto cn = child_cls->GetName();

        // Skip plumbing types.
        if (cn == L"WidgetTree" || cn == L"CanvasPanel" ||
            cn == L"VerticalBox" || cn == L"HorizontalBox" ||
            cn == L"Overlay"     || cn == L"Image"        ||
            cn == L"WidgetAnimation" || cn == L"InputComponent") {
            continue;
        }

        // Skip widgets that aren't currently visible — UE blueprints often
        // have hidden tooltip / description / template TextBlocks holding the
        // default "Text" placeholder string.
        if (!IsWidgetVisibleForReading(child)) continue;

        // TextBlock-derived widget: read its Text directly; recurse if the
        // class is a wrapper (BP_PalTextBlock_C, CommonTextBlock_C, ...) that
        // doesn't expose Text at the top level.
        const bool is_text =
            cn.find(L"TextBlock") != std::wstring::npos;
        if (is_text) {
            auto text = TryReadTextProperty(child);
            if (text.empty() && depth < 2) {
                text = ExtractWidgetLabels(child, depth + 1);
            }
            append(text);
            continue;
        }

        // Other UserWidget child (slider value display, switch state, dropdown
        // current value, etc.) — recurse to pick up its inner text and any
        // bool state it carries.
        if (depth < 2 && (cn.starts_with(L"WBP_") || cn.starts_with(L"BP_"))) {
            auto inner = ExtractWidgetLabels(child, depth + 1);
            // The inner extract may have its own state suffix; append as one.
            append(std::wstring_view(inner));
        }
    }

    if (!pending_state.empty()) {
        if (!result.empty()) result += L", ";
        result += pending_state;
    }
    return result;
}

// Generic "tagged button" lookup: walk Outer looking for any widget whose
// class ends in "Button_C" AND whose instance name starts with the class
// name minus "_C" (plus underscore). The OUTERMOST such ancestor is the most
// meaningful — that's the panel-author-named button rather than nested
// base classes.
//
// e.g. instance "WBP_Title_MenuButton_ExitGame_0" of class WBP_Title_MenuButton_C
//      → tag "ExitGame", class WBP_Title_MenuButton_C
struct TaggedButton {
    Unreal::UObject* widget    = nullptr;
    std::wstring     class_name;
    std::wstring     tag;
};

TaggedButton FindOutermostTaggedButton(Unreal::UObject* obj, int max_depth = 10) {
    TaggedButton best;
    Unreal::UObject* cur = obj;
    for (int d = 0; cur && d < max_depth; ++d) {
        std::wstring cn(ClassNameOf(cur));
        if (cn.ends_with(L"Button_C")) {
            auto inst = StripNumericSuffix(InstanceNameOf(cur));
            // If the instance name (after stripping a numeric suffix) is
            // just the class name itself, the widget had no semantic
            // designer-given name — its tag would be a meaningless "C".
            // Skip it so the row-extraction path runs.
            if (inst == cn) {
                cur = cur->GetOuterPrivate();
                continue;
            }
            // Class name without trailing "_C" + underscore.
            std::wstring expected = cn.substr(0, cn.size() - 2) + L"_";
            if (inst.starts_with(expected) && inst.size() > expected.size()) {
                auto candidate_tag = inst.substr(expected.size());
                // Reject trivial residuals (e.g. lone "C" if the instance
                // was just <Class>_N). They sound terrible spoken.
                if (candidate_tag != L"C" && !candidate_tag.empty()) {
                    best.widget     = cur;
                    best.class_name = std::move(cn);
                    best.tag        = std::move(candidate_tag);
                    // keep walking — outer match takes precedence
                }
            }
        }
        cur = cur->GetOuterPrivate();
    }
    return best;
}

// ---- Button text cache ----------------------------------------------------
// When WBP_CommonButton_C::SetText / SetDefaultText fires, we stash the FText
// keyed by the widget instance. When BP_OnHovered fires on a sub-button (e.g.
// WBP_PalInvisibleButton_C nested inside a WBP_CommonButton_C), we walk the
// Outer chain looking for a match.
class ButtonTextCache {
public:
    static ButtonTextCache& Get() {
        static ButtonTextCache c;
        return c;
    }

    void Set(Unreal::UObject* widget, std::wstring text) {
        if (!widget || text.empty()) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_map[reinterpret_cast<uintptr_t>(widget)] = std::move(text);
        // Cap memory — drop oldest-insertion bucket if we get too big.
        if (m_map.size() > 1024) {
            m_map.erase(m_map.begin());
        }
    }

    // Walks the Outer chain (max 8 levels) looking for a cached entry.
    std::wstring Lookup(Unreal::UObject* widget) {
        std::lock_guard<std::mutex> lock(m_mutex);
        Unreal::UObject* cur = widget;
        for (int depth = 0; cur && depth < 8; ++depth) {
            auto it = m_map.find(reinterpret_cast<uintptr_t>(cur));
            if (it != m_map.end()) return it->second;
            cur = cur->GetOuterPrivate();
        }
        return {};
    }

private:
    std::mutex                                m_mutex;
    std::unordered_map<uintptr_t, std::wstring> m_map;
};

// ---- Discovery logging ----------------------------------------------------

void LogScriptFunctionForDiscovery(Unreal::UObject* Context, Unreal::FFrame& Stack) {
    auto cls = Context->GetClassPrivate();
    if (!cls) return;
    auto class_name = cls->GetName();

    const bool looks_ui =
        class_name.starts_with(L"W_")              ||
        class_name.starts_with(L"WBP_")            ||
        class_name.starts_with(L"UMG_")            ||
        class_name.starts_with(L"UI_")             ||
        class_name.starts_with(L"BP_PalHUD")       ||
        class_name.starts_with(L"BP_PalPlayerController") ||
        class_name.find(L"Menu")         != std::wstring::npos ||
        class_name.find(L"Title")        != std::wstring::npos ||
        class_name.find(L"Focus")        != std::wstring::npos ||
        class_name.find(L"Button")       != std::wstring::npos ||
        class_name.find(L"Widget")       != std::wstring::npos ||
        class_name.find(L"Setting")      != std::wstring::npos ||
        class_name.find(L"Option")       != std::wstring::npos ||
        class_name.find(L"Server")       != std::wstring::npos ||
        class_name.find(L"World")        != std::wstring::npos ||
        class_name.find(L"Slider")       != std::wstring::npos ||
        class_name.find(L"Toggle")       != std::wstring::npos ||
        class_name.find(L"Dropdown")     != std::wstring::npos ||
        class_name.find(L"ComboBox")     != std::wstring::npos ||
        class_name.find(L"List")         != std::wstring::npos ||
        class_name.find(L"Slot")         != std::wstring::npos ||
        class_name.find(L"Build")        != std::wstring::npos ||
        class_name.find(L"Construction") != std::wstring::npos ||
        class_name.find(L"Placement")    != std::wstring::npos ||
        class_name.find(L"Production")   != std::wstring::npos ||
        class_name.find(L"Category")     != std::wstring::npos ||
        class_name.find(L"Tab")          != std::wstring::npos;
    if (!looks_ui) return;

    auto node = Stack.Node();
    if (!node) return;
    auto fn_name = node->GetName();

    if (fn_name == L"Tick" || fn_name == L"ReceiveTick" ||
        fn_name == L"BlueprintUpdateAnimation" ||
        fn_name == L"OnPaint") {
        return;
    }

    static std::mutex                       s_mtx;
    static std::unordered_set<std::wstring> s_seen;
    static int                              s_count = 0;
    std::wstring key = class_name + L"::" + fn_name;
    std::lock_guard<std::mutex> lock(s_mtx);
    if (s_count >= 2000) return;
    if (s_seen.insert(key).second) {
        ++s_count;
        Output::send<LogLevel::Verbose>(STR("[PalAccess] UFunc: {}\n"), key);
    }
}

// ---- Per-event handlers ---------------------------------------------------

// Cache button label text whenever it's set. We cache for any *Button_C /
// *_btn_C widget so that settings rows, dropdowns, dialog buttons, etc. all
// pick up their labels.
void HandleButtonSetText(Unreal::UObject* Context, Unreal::FFrame& Stack) {
    auto class_name = ClassNameOf(Context);
    const bool is_button = class_name.ends_with(L"Button_C") ||
                           class_name.ends_with(L"_btn_C")   ||
                           class_name.find(L"Button") != std::wstring::npos;
    if (!is_button) return;
    if (!FunctionNameIs(Stack, L"SetText") &&
        !FunctionNameIs(Stack, L"SetDefaultText") &&
        !FunctionNameIs(Stack, L"SetMainText") &&
        !FunctionNameIs(Stack, L"SetValue")) {
        return;
    }
    auto str = ReadFirstFTextParam(Stack);
    if (str.empty()) return;

    ButtonTextCache::Get().Set(Context, std::wstring(str));
    Output::send<LogLevel::Verbose>(
        STR("[PalAccess] cache button text: {} = \"{}\"\n"),
        class_name, str);
}

// User hovered or focused a button. Triggered by any of the focus-style
// animation events Palworld fires:
//   BP_OnHovered     — mouse hover, UE5 CommonUI default
//   AnmEvent_Focus   — animation hook for keyboard / controller focus
//   AnmEvent_Hover   — used by construction-menu icons and similar
void HandleButtonHover(Unreal::UObject* Context, Unreal::FFrame& Stack) {
    if (!FunctionNameIs(Stack, L"BP_OnHovered")    &&
        !FunctionNameIs(Stack, L"AnmEvent_Focus")  &&
        !FunctionNameIs(Stack, L"AnmEvent_Hover")) return;
    // Construction icons get special-cased by HandleConstructionItemFocus
    // which reads the side info panel; otherwise the generic hover path
    // here would just announce "Construction" (the humanized class name).
    if (ClassNameOf(Context) == L"WBP_IngameMenu_Construction_Icon_C") return;

    // 1) Dynamic SetText cache — handles popup buttons like "Close", "OK".
    if (auto label = ButtonTextCache::Get().Lookup(Context); !label.empty()) {
        Speech::Get().Speak(std::wstring_view(label));
        return;
    }

    // 2) Generic tagged-button lookup: works for WBP_Title_MenuButton_C,
    //    WBP_Title_SettingsButton_C, WBP_Title_WorldSelectButton_C,
    //    WBP_GuildHeadButton_C, and any future *Button_C with semantic
    //    instance names.
    if (auto tb = FindOutermostTaggedButton(Context); tb.widget) {
        Output::send<LogLevel::Verbose>(
            STR("[PalAccess] tagged button: {} :: {}\n"),
            tb.class_name, tb.tag);
        if (auto friendly = LookupButtonLabel(tb.tag); !friendly.empty()) {
            Speech::Get().FocusUpdate(friendly);
            return;
        }
        if (!tb.tag.empty()) {
            Speech::Get().FocusUpdate(std::wstring_view(Humanize(tb.tag)));
            return;
        }
    }

    // 3) Reflection: read the containing row widget's label text directly.
    //    Works for settings rows, list entries, anything whose UserWidget
    //    exposes named TextBlocks or FText UProperties.
    if (auto* row = FindContainingRowWidget(Context)) {
        // One-shot dump per class so we can see what's actually in unknown
        // widgets when extraction fails.
        DumpPropertiesOnce(row);
        auto extracted = ExtractWidgetLabels(row);
        if (!extracted.empty()) {
            Output::send<LogLevel::Verbose>(
                STR("[PalAccess] row labels: {} = \"{}\"\n"),
                ClassNameOf(row), extracted);
            Speech::Get().FocusUpdate(std::wstring_view(extracted));
            return;
        }
    }

    // 4) Friendly-class fallback for known classes.
    if (auto* uw = OutermostUserWidget(Context)) {
        auto cls_name = ClassNameOf(uw);
        Output::send<LogLevel::Verbose>(
            STR("[PalAccess] hover user-widget: {} instance={}\n"),
            cls_name, InstanceNameOf(uw));
        if (auto friendly = FriendlyNameFor(cls_name); !friendly.empty()) {
            Speech::Get().FocusUpdate(friendly);
            return;
        }
        auto inst = StripNumericSuffix(InstanceNameOf(uw));
        std::wstring spoken = inst.empty() ? cls_name : Humanize(inst);
        Speech::Get().FocusUpdate(std::wstring_view(spoken));
        return;
    }

    Speech::Get().FocusUpdate(std::wstring_view(ClassNameOf(Context)));
}

// A major UI panel was constructed — announce the screen change.
void HandleScreenArrival(Unreal::UObject* Context, Unreal::FFrame& Stack) {
    if (!FunctionNameIs(Stack, L"Construct") &&
        !FunctionNameIs(Stack, L"OnSetup")) {
        return;
    }
    struct Entry { std::wstring_view klass; std::wstring_view spoken; };
    static const Entry kPanels[] = {
        // Confirmed in earlier logs
        { L"WBP_TItle_C",                       L"Title screen"            },
        { L"WBP_TitleMenu_C",                   L"Title menu"              },
        { L"WBP_Title_WorldSelect_C",           L"World select"            },
        { L"WBP_JoinGame_C",                    L"Join game"               },
        { L"WBP_PalDialog_C",                   L"Dialog"                  },
        { L"WBP_CommonPopupWindow_C",           L"Pop-up window"           },
        { L"WBP_LoadingScreen_C",               L"Loading"                 },
        // Confirmed Settings hierarchy
        { L"WBP_OptionSettings_C",              L"Settings"                },
        { L"WBP_OptionSettings_ListContent_C",  L"Settings content"        },
        { L"WBP_Graphic_Settings_C",            L"Graphics settings"       },
        { L"WBP_Sound_Settings_C",              L"Audio settings"          },
        { L"WBP_Control_Settings_C",            L"Controls settings"       },
        { L"WBP_Key_Settings_C",                L"Key bindings"            },
        { L"WBP_Other_Settings_C",              L"Other settings"          },
        // Confirmed world-select / create-world panels
        { L"WBP_TitleLocalWorldSelect_C",       L"Local world select"      },
        { L"WBP_Title_WorldSettings_C",         L"World settings"          },
        { L"WBP_WorldSetting_C",                L"World settings"          },
        { L"WBP_Title_WorldSelect_CreateWorld_ListContent_C", L"Create new world" },
        // Construction / build / placement menus (D-pad up during gameplay).
        // Names are guesses informed by Palworld's naming conventions;
        // unknown ones still get the generic fallback below.
        { L"WBP_Build_C",                       L"Build menu"              },
        { L"WBP_BuildMenu_C",                   L"Build menu"              },
        { L"WBP_BuildList_C",                   L"Build menu"              },
        { L"WBP_PalBuildMenu_C",                L"Build menu"              },
        { L"WBP_PalBuildMode_C",                L"Build mode"              },
        { L"WBP_PalBuilder_C",                  L"Build menu"              },
        { L"WBP_PalBuilderRoot_C",              L"Build menu"              },
        { L"WBP_Construction_C",                L"Construction menu"       },
        { L"WBP_ConstructionMenu_C",            L"Construction menu"       },
        { L"WBP_Placement_C",                   L"Placement menu"          },
        { L"WBP_PlacementMenu_C",               L"Placement menu"          },
        { L"WBP_QuickAccess_Build_C",           L"Build quick menu"        },
        // Still-guessing entries for single-player / multiplayer flows
        { L"WBP_NewWorld_C",                    L"Create new world"        },
        { L"WBP_CreateWorld_C",                 L"Create new world"        },
        { L"WBP_NewGame_C",                     L"New game"                },
        { L"WBP_WorldSelect_C",                 L"World list"              },
        { L"WBP_LocalWorldList_C",              L"Local worlds"            },
        { L"WBP_WorldList_C",                   L"World list"              },
        { L"WBP_WorldSettings_C",               L"World settings"          },
        { L"WBP_WorldDetail_C",                 L"World details"           },
        { L"WBP_WorldEdit_C",                   L"Edit world"              },
        { L"WBP_PalWorldSettings_C",            L"World settings"          },
        { L"WBP_GameSettings_C",                L"Game settings"           },
        { L"WBP_CloudSave_C",                   L"Cloud saves"             },
        { L"WBP_ServerList_C",                  L"Server list"             },
        { L"WBP_Title_ServerList_C",            L"Server list"             },
        { L"WBP_Title_InviteCode_C",            L"Invite code"             },
        { L"WBP_InviteCode_C",                  L"Invite code"             },
        { L"WBP_Title_HostServer_C",            L"Host server"             },
    };
    auto cls = Context->GetClassPrivate();
    if (!cls) return;
    auto name = cls->GetName();
    // Cache HUD gauge widgets so the F1/F2/F3 hotkeys can read their
    // displayed cur/max text even when SaveParameter's max is unpopulated.
    Hotkeys::NoticePotentialGaugeWidget(Context);

    for (const auto& e : kPanels) {
        if (name == e.klass) {
            Speech::Get().Announce(std::wstring_view(e.spoken));
            return;
        }
    }

    // Generic catch-all: any unknown class whose name contains "Build",
    // "Construction", or "Placement" — almost certainly the construction
    // popup or a sub-panel of it. Speak something useful even before we
    // map the exact class name.
    if (FunctionNameIs(Stack, L"Construct") &&
        !name.ends_with(L"Button_C") &&
        !name.ends_with(L"_btn_C") &&
        !name.ends_with(L"Icon_C") &&
        !name.ends_with(L"Slot_C")) {
        static std::mutex                       s_b_mtx;
        static std::unordered_set<std::wstring> s_b_seen;
        const bool looks_build =
            (name.find(L"Build")        != std::wstring::npos ||
             name.find(L"Construction") != std::wstring::npos ||
             name.find(L"Placement")    != std::wstring::npos) &&
            (name.starts_with(L"WBP_") || name.starts_with(L"W_"));
        if (looks_build) {
            std::lock_guard<std::mutex> lock(s_b_mtx);
            if (s_b_seen.insert(name).second) {
                Output::send<LogLevel::Default>(
                    STR("[PalAccess] build-shaped construct: {}\n"), name);
            }
            // Announce category each time the widget reappears so the user
            // knows they're in the build flow.
            std::wstring spoken = L"Build menu";
            if (name.find(L"Placement") != std::wstring::npos) spoken = L"Placement menu";
            else if (name.find(L"Construction") != std::wstring::npos) spoken = L"Construction menu";
            Speech::Get().Announce(std::wstring_view(spoken));
            return;
        }
    }

    // Discovery aid: log the FIRST construct call for any class that LOOKS
    // panel-shaped (top-level WBP_*) so we can map new screens as they appear.
    // Limit to classes that don't look like a sub-widget (no nested suffixes).
    if (FunctionNameIs(Stack, L"Construct") &&
        (name.starts_with(L"WBP_") || name.starts_with(L"W_")) &&
        !name.ends_with(L"Button_C") &&
        !name.ends_with(L"Icon_C") &&
        !name.ends_with(L"_btn_C") &&
        !name.ends_with(L"Cursor_C")) {
        static std::mutex                       s_mtx;
        static std::unordered_set<std::wstring> s_seen;
        static int                              s_count = 0;
        std::lock_guard<std::mutex> lock(s_mtx);
        if (s_count < 200 && s_seen.insert(name).second) {
            ++s_count;
            Output::send<LogLevel::Verbose>(
                STR("[PalAccess] panel Construct: {}\n"), name);
        }
    }
}

// A click happened — give the user feedback. Strategy:
//  1) Cached SetText label (popup buttons) → "Selected <label>"
//  2) Tagged-button label (title-menu / settings categories) → "Selected <label>"
//  3) Settings-row click (changes a value): re-read the row via reflection so
//     the user hears the new state ("Mouse Sensitivity, 2"). No "Selected"
//     prefix — the announcement is the new value itself.
//  4) Fallback to humanized outer-widget class name.
void HandleButtonClick(Unreal::UObject* Context, Unreal::FFrame& Stack) {
    if (!FunctionNameIs(Stack, L"BP_OnClicked")) return;

    // 1) Cached label.
    if (auto cached = ButtonTextCache::Get().Lookup(Context); !cached.empty()) {
        std::wstring msg = L"Selected ";
        msg += cached;
        Speech::Get().Announce(std::wstring_view(msg));
        return;
    }

    // 2) Tagged button.
    if (auto tb = FindOutermostTaggedButton(Context); tb.widget) {
        auto friendly = LookupButtonLabel(tb.tag);
        std::wstring label = friendly.empty() ? Humanize(tb.tag)
                                              : std::wstring(friendly);
        std::wstring msg = L"Selected ";
        msg += label;
        Speech::Get().Announce(std::wstring_view(msg));
        return;
    }

    // 3) Settings row: re-extract label + new value. Announce as the new
    //    state. FocusUpdate (not Announce) so it interrupts cleanly and
    //    plays the new value tightly after the prior speech.
    if (auto* row = FindContainingRowWidget(Context)) {
        auto extracted = ExtractWidgetLabels(row);
        if (!extracted.empty()) {
            Output::send<LogLevel::Verbose>(
                STR("[PalAccess] click row state: {} = \"{}\"\n"),
                ClassNameOf(row), extracted);
            Speech::Get().FocusUpdate(std::wstring_view(extracted));
            return;
        }
    }

    // 4) Humanized fallback.
    std::wstring label;
    if (auto* uw = OutermostUserWidget(Context)) {
        auto cls_name = ClassNameOf(uw);
        auto friendly = FriendlyNameFor(cls_name);
        label = friendly.empty() ? Humanize(cls_name) : std::wstring(friendly);
    }
    std::wstring msg = L"Selected ";
    msg += label.empty() ? L"button" : label;
    Speech::Get().Announce(std::wstring_view(msg));
}

// Designer-time placeholder fragments seen in Palworld widgets. If a
// SetText call carries any of these, it's the template default — not the
// real runtime message. Skip.
bool IsNotificationPlaceholder(std::wstring_view t) {
    if (t.empty()) return true;
    constexpr std::wstring_view kPlaceholders[] = {
        L"あいうえお", L"ABcdEFgh", L"ほげほげ", L"hogehoge",
        L"（仮）", L"Lorem ipsum", L"未完了は赤", L"パル名前",
    };
    for (auto p : kPlaceholders) {
        if (t.find(p) != std::wstring::npos) return true;
    }
    return false;
}

// Cache the construction-menu info-panel widget so we can re-read it on
// every item-focus change (where Setup doesn't re-fire, only the inner
// TextBlocks get updated).
Unreal::UObject* g_construction_info_panel = nullptr;
std::mutex                                    g_construction_mtx;
std::wstring                                  g_construction_last;
std::chrono::steady_clock::time_point         g_construction_last_t{};

// The parent panel — shows the build object's NAME + description. This is
// what we want to announce. InfoItem (single material row) is its child and
// just shows numbers like "6, 2, Wood".
bool IsConstructionParentInfo(std::wstring_view cn) {
    return cn == L"WBP_IngameMenu_Construction_Info_C" ||
           cn == L"WBP_InGameMenu_Construction_Info_C";
}

bool IsConstructionInfoPanel(std::wstring_view cn) {
    return IsConstructionParentInfo(cn);
}

// Strip Palworld's inline rich-text tags (e.g. `<mapObjectName id="WorkBench"/>`)
// and collapse whitespace so the read-out doesn't trip over markup or
// embedded newlines.
std::wstring StripRichTextMarkup(std::wstring_view s) {
    std::wstring tmp;
    tmp.reserve(s.size());
    bool in_tag = false;
    for (wchar_t c : s) {
        if (c == L'<') { in_tag = true; continue; }
        if (c == L'>') { in_tag = false; continue; }
        if (in_tag) continue;
        if (c == L'\n' || c == L'\r' || c == L'\t') c = L' ';
        tmp.push_back(c);
    }
    // Collapse runs of whitespace and trim.
    std::wstring out;
    out.reserve(tmp.size());
    bool prev_space = true;
    for (wchar_t c : tmp) {
        if (c == L' ') {
            if (!prev_space) out.push_back(c);
            prev_space = true;
        } else {
            out.push_back(c);
            prev_space = false;
        }
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    return out;
}

std::wstring ReadChildTextOrLabel(Unreal::UObject* child) {
    if (!child) return {};
    if (!IsWidgetVisibleForReading(child)) return {};
    auto t = TryReadTextProperty(child);
    if (t.empty()) t = ExtractWidgetLabels(child, 1);
    return t;
}

// Focused extractor for the construction info panel: whitelists only the
// item name and description widgets (`RichText_Name`, `RichText_Desc`).
// Everything else on the panel — favorite-toggle button, man-month build
// time, caution text, and the nested InfoItem cost rows — is skipped.
std::wstring ExtractConstructionInfoText(Unreal::UObject* widget) {
    if (!widget) return {};
    auto cls = widget->GetClassPrivate();
    if (!cls) return {};
    Unreal::UObject* name_child = nullptr;
    Unreal::UObject* desc_child = nullptr;
    for (auto* prop : cls->ForEachPropertyInChain()) {
        if (!prop) continue;
        if (prop->GetClass().GetName() != L"ObjectProperty") continue;
        auto pname = prop->GetName();
        if (pname != L"RichText_Name" && pname != L"RichText_Desc") continue;
        auto** ptr = prop->ContainerPtrToValuePtr<Unreal::UObject*>(widget);
        if (!ptr || !*ptr) continue;
        if (pname == L"RichText_Name") name_child = *ptr;
        else                            desc_child = *ptr;
    }
    std::wstring name = name_child ? StripRichTextMarkup(ReadChildTextOrLabel(name_child)) : L"";
    std::wstring desc = desc_child ? StripRichTextMarkup(ReadChildTextOrLabel(desc_child)) : L"";
    if (IsPlaceholderText(name)) name.clear();
    if (IsPlaceholderText(desc)) desc.clear();
    if (name.empty() && desc.empty()) return {};
    if (name.empty()) return desc;
    if (desc.empty()) return name;
    return name + L". " + desc;
}

void AnnounceConstructionInfoFromPanel(Unreal::UObject* panel) {
    if (!panel) return;
    DumpPropertiesOnce(panel);

    auto txt = ExtractConstructionInfoText(panel);
    if (txt.empty() || IsNotificationPlaceholder(txt)) return;
    if (txt.size() > 400) return;

    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_construction_mtx);
        if (txt == g_construction_last &&
            now - g_construction_last_t < std::chrono::milliseconds(300)) return;
        g_construction_last   = txt;
        g_construction_last_t = now;
    }
    Output::send<LogLevel::Verbose>(
        STR("[PalAccess] construction info: \"{}\"\n"), txt);
    Speech::Get().FocusUpdate(std::wstring_view(txt));
}

// Cache the parent Info panel pointer when one of its update functions
// fires. `Construction_Info_C` populates with the build object's name and
// description via SetBuildObjectData → UpdateDetail → DelayDisplay; any of
// those is a good moment to re-read its TextBlocks.
void HandleConstructionInfoUpdate(Unreal::UObject* Context, Unreal::FFrame& Stack) {
    if (!Context) return;
    auto cn = ClassNameOf(Context);
    if (!IsConstructionInfoPanel(cn)) return;
    if (!FunctionNameIs(Stack, L"Setup") &&
        !FunctionNameIs(Stack, L"UpdateDetail") &&
        !FunctionNameIs(Stack, L"DelayDisplay") &&
        !FunctionNameIs(Stack, L"SetBuildObjectData") &&
        !FunctionNameIs(Stack, L"UpdateInfo") &&
        !FunctionNameIs(Stack, L"OnUpdate")) return;

    g_construction_info_panel = Context;
    AnnounceConstructionInfoFromPanel(Context);
}

// Material cost row inside the construction info panel. Each row carries
// three text fields: "<have>, <need>, <material name>" (or possibly the
// other order — both read sensibly with "X of Y Material"). Formatted into
// a single line and queued so consecutive material lines play in order
// after the item name.
void HandleConstructionMaterialRow(Unreal::UObject* Context, Unreal::FFrame& Stack) {
    if (!Context) return;
    auto cn = ClassNameOf(Context);
    if (cn != L"WBP_IngameMenuConstruction_InfoItem_C" &&
        cn != L"WBP_InGameMenuConstruction_InfoItem_C") return;
    if (!FunctionNameIs(Stack, L"Setup") &&
        !FunctionNameIs(Stack, L"UpdateDetail") &&
        !FunctionNameIs(Stack, L"OnUpdate")) return;

    auto raw = ExtractWidgetLabels(Context, 0);
    if (raw.empty() || IsNotificationPlaceholder(raw)) return;

    // Split on ", " — ExtractWidgetLabels joins with that exact separator.
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start <= raw.size()) {
        auto pos = raw.find(L", ", start);
        if (pos == std::wstring::npos) {
            parts.emplace_back(raw.substr(start));
            break;
        }
        parts.emplace_back(raw.substr(start, pos - start));
        start = pos + 2;
    }
    if (parts.size() < 3) return;

    // "<have>, <need>, <material>" → "<have> of <need> <material>"
    std::wstring formatted = parts[0] + L" of " + parts[1] + L" " + parts[2];

    // Dedupe same line within 1 s (multiple Setup fires per row are common).
    static std::mutex                                                       s_mtx;
    static std::unordered_map<std::wstring, std::chrono::steady_clock::time_point> s_recent;
    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(s_mtx);
        auto it = s_recent.find(formatted);
        if (it != s_recent.end() && now - it->second < std::chrono::seconds(1)) return;
        s_recent[formatted] = now;
        if (s_recent.size() > 64) {
            for (auto i = s_recent.begin(); i != s_recent.end();) {
                if (now - i->second > std::chrono::seconds(10)) i = s_recent.erase(i);
                else ++i;
            }
        }
    }
    Output::send<LogLevel::Verbose>(
        STR("[PalAccess] construction material: \"{}\"\n"), formatted);
    Speech::Get().Queue(std::wstring_view(formatted));
}

// Tab selection: AnmEvent_Select fires on Construction_Tab_C when the user
// switches between categories (Production / Foundation / Logistics / etc.).
// Read the tab's own TextBlocks for the category name.
void HandleConstructionTabSelect(Unreal::UObject* Context, Unreal::FFrame& Stack) {
    if (!Context) return;
    if (ClassNameOf(Context) != L"WBP_IngameMenu_Construction_Tab_C") return;
    if (!FunctionNameIs(Stack, L"AnmEvent_Select")) return;
    DumpPropertiesOnce(Context);
    auto txt = ExtractWidgetLabels(Context, 0);
    if (txt.empty() || IsNotificationPlaceholder(txt)) return;
    if (txt.size() > 120) return;
    Output::send<LogLevel::Verbose>(
        STR("[PalAccess] construction tab: \"{}\"\n"), txt);
    Speech::Get().Announce(std::wstring_view(txt));
}

// Item navigation: when an icon is hovered or the Group fires its bound
// "any icon hovered" delegate, re-read the cached info panel — that's
// where the actual item name/description has just been written.
void HandleConstructionItemFocus(Unreal::UObject* Context, Unreal::FFrame& Stack) {
    if (!Context) return;
    auto fn = Stack.Node() ? Stack.Node()->GetName() : std::wstring();
    auto cn = ClassNameOf(Context);
    const bool is_focus_event =
        (cn == L"WBP_IngameMenu_Construction_Icon_C" &&
         (fn == L"AnmEvent_Hover" || fn == L"AnmEvent_Focus" ||
          fn == L"BP_OnHovered")) ||
        (cn == L"WBP_IngameMenu_Construction_Group_C" &&
         fn.find(L"OnHoveredAnyBuildObjectIcon") != std::wstring::npos);
    if (!is_focus_event) return;
    AnnounceConstructionInfoFromPanel(g_construction_info_panel);
}

// System notifications — level-up, quest add/complete, achievements,
// rewards, autosave, tutorials. Reacts only to explicit SetText-style
// calls; Construct extraction is too greedy and leaks designer
// placeholder strings.
void HandleSystemNotification(Unreal::UObject* Context, Unreal::FFrame& Stack) {
    auto cn = ClassNameOf(Context);
    auto has = [&](std::wstring_view needle) {
        return cn.find(needle) != std::wstring::npos;
    };
    const bool is_notif =
        has(L"Notice")       || has(L"Notification")  ||
        has(L"LevelUp")      || has(L"Quest")         ||
        has(L"Achievement")  || has(L"Unlock")        ||
        has(L"Banner")       || has(L"Toast")         ||
        has(L"Reward")       || has(L"AutoSave")      ||
        has(L"Tutorial");
    if (!is_notif) return;
    // Skip sub-widgets that belong to UI we already handle.
    if (cn.starts_with(L"WBP_InventoryEquipment")) return;
    if (cn.starts_with(L"WBP_Menu_")) return;

    auto fn = Stack.Node() ? Stack.Node()->GetName() : std::wstring();

    std::wstring t;
    if (fn == L"SetText"     || fn == L"SetMessage"  || fn == L"SetMainText" ||
        fn == L"SetTitle"    || fn == L"SetCaption"  || fn == L"SetContent"  ||
        fn == L"SetDescription") {
        t = ReadFirstFTextParam(Stack);
    } else if (fn == L"Construct" || fn == L"OnSetup" || fn == L"OnInitialized") {
        // Many notifications don't route through SetText — they just bind
        // text via data and fire Construct. Extract visible TextBlocks,
        // then apply heuristics to throw out widget-template dumps.
        auto raw = ExtractWidgetLabels(Context, 0);
        if (raw.empty()) return;
        if (raw.size() > 240) return;                           // too long
        // Count ", " fragments — single-sentence notifications have 0 or 1.
        int fragments = 0;
        for (size_t i = 0; i + 1 < raw.size(); ++i) {
            if (raw[i] == L',' && raw[i + 1] == L' ') ++fragments;
        }
        if (fragments > 2) return;
        t = std::move(raw);
    } else {
        return;
    }
    if (t.empty() || IsNotificationPlaceholder(t)) return;

    // Dedupe identical messages fired within 1 s — some widgets call
    // SetText multiple times in a single frame when populating.
    static std::mutex                              s_dmtx;
    static std::wstring                            s_last;
    static std::chrono::steady_clock::time_point   s_last_t{};
    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(s_dmtx);
        if (t == s_last && now - s_last_t < std::chrono::seconds(1)) return;
        s_last   = t;
        s_last_t = now;
    }

    Output::send<LogLevel::Verbose>(
        STR("[PalAccess] notify: {} via {} = \"{}\"\n"), cn, fn, t);
    Speech::Get().Announce(std::wstring_view(t));
}

// Dialog body text. Only hook SetMainText: it has a known FText signature.
// SetupUI on WBP_PalDialog_C has a different parameter layout (a config
// struct, not FText) and blind-casting caused a crash on Confirm in world
// settings.
void HandleDialogText(Unreal::UObject* Context, Unreal::FFrame& Stack) {
    if (!ClassNameStartsWith(Context, L"WBP_CommonPopupWindow") &&
        !ClassNameStartsWith(Context, L"WBP_PalDialog")) {
        return;
    }
    if (!FunctionNameIs(Stack, L"SetMainText")) return;
    auto str = ReadFirstFTextParam(Stack);
    if (!str.empty()) {
        Speech::Get().Announce(std::wstring_view(str));
    }
}

} // namespace

// ---- installation ----------------------------------------------------------

// Public reader so Hotkeys.cpp can extract gauge-widget text without
// duplicating the reflection logic.
std::wstring Hooks::ExtractAllText(Unreal::UObject* widget) {
    return ExtractWidgetLabels(widget, 0);
}

void Hooks::Install() {
    Unreal::Hook::RegisterBeginPlayPostCallback(&OnActorBeginPlay);
    Unreal::Hook::RegisterProcessLocalScriptFunctionPostCallback(&OnPostScriptFunction);
    Output::send<LogLevel::Default>(STR("[PalAccess] Hooks installed\n"));
}

// ---- BeginPlay -------------------------------------------------------------

void Hooks::OnActorBeginPlay(Unreal::AActor* Context) {
    if (!Context) return;

    static std::mutex                       s_mtx;
    static std::unordered_set<std::wstring> s_seen;
    if (auto cls = Context->GetClassPrivate()) {
        auto name = cls->GetName();
        std::lock_guard<std::mutex> lock(s_mtx);
        if (s_seen.insert(name).second) {
            Output::send<LogLevel::Verbose>(STR("[PalAccess] BeginPlay: {}\n"), name);
        }
    }
    Hotkeys::NoticePotentialPlayer(Context);

    if (ClassNameStartsWith(Context, L"BP_PalPlayerController_Title")) {
        // Fires once when the title-screen player controller arrives.
        // Slight delay would be nice but we don't have a timer; the speech
        // dedup makes a repeated greet harmless.
        Speech::Get().Announce(L"Welcome to Palworld. Main menu loading.");
    }
}

// ---- ProcessLocalScriptFunction post-callback ------------------------------

PALACCESS_UFUNC_HOOK(Hooks::OnPostScriptFunction) {
    if (!Context) return;
    LogScriptFunctionForDiscovery(Context, Stack);
    HandleButtonSetText(Context, Stack);
    HandleScreenArrival(Context, Stack);
    HandleButtonHover(Context, Stack);
    HandleButtonClick(Context, Stack);
    HandleDialogText(Context, Stack);
    HandleSystemNotification(Context, Stack);
    HandleConstructionInfoUpdate(Context, Stack);
    HandleConstructionItemFocus(Context, Stack);
    HandleConstructionMaterialRow(Context, Stack);
    HandleConstructionTabSelect(Context, Stack);
}

// Legacy placeholder dispatchers kept as no-ops — declared in the header so the
// translation unit still satisfies the interface. Replaced by the named handlers above.
void Hooks::TryAnnounceDialog(Unreal::UObject*, Unreal::FFrame&)      {}
void Hooks::TryAnnounceMenuFocus(Unreal::UObject*, Unreal::FFrame&)   {}
void Hooks::TryAnnounceItemPickup(Unreal::UObject*, Unreal::FFrame&)  {}
void Hooks::TryAnnouncePalEvent(Unreal::UObject*, Unreal::FFrame&)    {}

} // namespace PalAccess
