#include "Hooks.hpp"
#include "Speech.hpp"
#include "Hotkeys.hpp"
#include "PatchNotes.hpp"

#include <DynamicOutput/Output.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/String.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>

#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

using namespace RC;

namespace PalAccess {

// File-scope state for the patch-notes WebBrowser URL poll. Populated
// when WBP_WebBrowser_News_C constructs; cleared once we log the URL
// or the cached pointer dies.
static std::mutex                                   g_news_mtx;
static Unreal::UObject*                             g_news_inner = nullptr;
static bool                                         g_news_url_logged = false;
static std::chrono::steady_clock::time_point        g_news_construct_at{};

// File-scope state for the quest start/clear popup poll. Populated when
// WBP_InGame_Quest_StartClear_C constructs; cleared once we successfully
// announce the resolved quest title or the cached pointer dies.
//
// Why polling: Palworld constructs the popup with placeholder text
// ("クエスト名") then the widget's ubergraph resolves the title from its
// quest DataTable. The text-block SetText path doesn't reliably fire
// through the blueprint UFunction hook (it's set via FText binding or
// native code). Reading the live FText value off the text-block
// property each tick is the most robust path.
static std::mutex                                   g_quest_mtx;
static Unreal::UObject*                             g_quest_popup = nullptr;
static std::chrono::steady_clock::time_point        g_quest_construct_at{};
static bool                                         g_quest_announced = false;
// Set by HandleQuestNotification when the questboard tracker updates —
// used as a fallback source for the resolved title when the popup keeps
// rendering the クエスト名 placeholder.
static std::wstring                                 g_questboard_labels;
static std::chrono::steady_clock::time_point        g_questboard_at{};

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
    { L"WBP_Title_SettingsButton_C",    L"Settings"            },
    { L"WBP_Title_WorldSelectButton_C", L"World select button" },
    { L"WBP_Title_WorldMenu_Head_C",    L"World menu"          },
    { L"WBP_Title_WorldSelect_C",       L"World select panel"  },
    { L"WBP_TitleVerText_C",            L"Version info"        },
    // WBP_Title_MenuButton_C, WBP_TItle_C, and WBP_TitleMenu_C
    // removed: their friendly names ("Title menu button", "Title
    // screen", "Title menu") leak out as hover fallbacks before the
    // patch-notes screen settles, drowning out the patch notes.
    // Specific instance-tagged buttons (StartLocalGame, etc.) still
    // speak via FindOutermostTaggedButton.
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

// Forward declarations for helpers defined later in the same TU; needed
// because HandleButtonHover (below) post-processes the extracted row
// labels with these.
std::wstring        StripRichTextMarkup(std::wstring_view s);
std::wstring        HumanizeRichTextId(std::wstring_view id);
bool                IsNotificationPlaceholder(std::wstring_view t);
Unreal::UFunction*  FindFunctionInChain(Unreal::UObject* obj,
                                        std::wstring_view fname);

// Last hovered recipe slot — used by the click handler to know which
// recipe's materials to read when the player activates one.
Unreal::UObject*                              g_last_recipe_slot = nullptr;
std::chrono::steady_clock::time_point         g_last_recipe_slot_t;

// Shared dedup state for recipe-name announces. Lifted to file scope so
// the click handler can mark a name as "just spoken" — that suppresses
// the hover handler's rapid re-fire when Palworld's click flow re-calls
// GetRecipeId several times in quick succession.
std::mutex                                    g_recipe_speech_mtx;
std::wstring                                  g_recipe_last_msg;
std::chrono::steady_clock::time_point         g_recipe_last_msg_t;

// Slot that the materials line was last announced for. Reset to nullptr
// whenever the hover handler observes the user moving to a different
// slot. Lets the click handler skip Palworld's repeated
// CommonButtonBaseClicked fires on the same focused slot, while still
// re-announcing when the user navigates away and returns.
Unreal::UObject*                              g_click_announced_slot = nullptr;

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
    // Inventory item slot buttons handled by HandleInventorySlotFocus.
    // Hover/focus events fire on the slot button itself AND on its inner
    // child widgets (WBP_PalInvisibleButton_C, etc.). Walk up the outer
    // chain to detect the slot button at any depth so the generic hover
    // path doesn't double-announce on top of HandleInventorySlotFocus.
    if (auto* uw = OutermostUserWidget(Context); uw) {
        auto uw_cls = ClassNameOf(uw);
        if (uw_cls == L"WBP_PalInGameMenuItemSlotButton_C" ||
            uw_cls == L"WBP_PalConvertItemMenu_RecipeSlotButton_C") {
            return;
        }
    }

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
            // Strip Palworld's rich-text markup (<itemName id="..."/>,
            // <mapObjectName id="..."/>) and collapse consecutive
            // duplicate ", "-separated fragments. Without this, tech
            // tree rows speak as
            // "Structures, <mapObjectName id=\"WorkBench\"/>, <mapObjectName id=\"WorkBench\"/>, 前提未取得, 1"
            std::wstring cleaned = StripRichTextMarkup(extracted);
            // Split, drop placeholders + adjacent duplicates, rejoin.
            std::vector<std::wstring> parts;
            size_t start = 0;
            while (start <= cleaned.size()) {
                auto pos = cleaned.find(L", ", start);
                std::wstring frag = (pos == std::wstring::npos)
                    ? cleaned.substr(start)
                    : cleaned.substr(start, pos - start);
                // Trim leading/trailing whitespace from each fragment.
                while (!frag.empty() && (frag.front() == L' ' || frag.front() == L'\t')) frag.erase(frag.begin());
                while (!frag.empty() && (frag.back()  == L' ' || frag.back()  == L'\t')) frag.pop_back();
                if (!frag.empty() &&
                    !IsNotificationPlaceholder(frag) &&
                    (parts.empty() || parts.back() != frag)) {
                    parts.push_back(std::move(frag));
                }
                if (pos == std::wstring::npos) break;
                start = pos + 2;
            }
            std::wstring final_text;
            for (size_t i = 0; i < parts.size(); ++i) {
                if (i) final_text += L", ";
                final_text += parts[i];
            }
            if (final_text.empty()) return;
            Speech::Get().FocusUpdate(std::wstring_view(final_text));
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
    // Patch-notes Destruct: cancel any in-flight fetch + silence ongoing
    // reading. Done before the Construct/OnSetup gate so Destruct events
    // reach this branch.
    if (FunctionNameIs(Stack, L"Destruct")) {
        auto cls = Context->GetClassPrivate();
        if (cls && cls->GetName() == L"WBP_WebBrowser_News_C") {
            PatchNotes::Cancel();
        }
        return;
    }
    if (!FunctionNameIs(Stack, L"Construct") &&
        !FunctionNameIs(Stack, L"OnSetup")) {
        return;
    }
    struct Entry { std::wstring_view klass; std::wstring_view spoken; };
    static const Entry kPanels[] = {
        // Title screen (WBP_TItle_C), title menu container
        // (WBP_TitleMenu_C), and loading screen (WBP_LoadingScreen_C)
        // intentionally omitted: they construct during game launch and
        // would either compete with the patch-notes announce in the
        // cluster window or speak after dismissal as redundant noise.
        // The first focused menu button announces itself on focus.
        { L"WBP_Title_WorldSelect_C",           L"World select"            },
        { L"WBP_JoinGame_C",                    L"Join game"               },
        { L"WBP_PalDialog_C",                   L"Dialog"                  },
        { L"WBP_CommonPopupWindow_C",           L"Pop-up window"           },
        // Confirmed Settings hierarchy. The container panels
        // (WBP_OptionSettings_C / WBP_OptionSettings_ListContent_C)
        // intentionally omitted — they construct alongside all five
        // tab panels in a cluster, and the user really wants to hear
        // the active tab ("Graphics settings"), not the parent.
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
        // Confirmed via UE4SS.log discovery: Palworld renders the
        // pre-title patch notes inside an embedded Chromium browser
        // (WBP_WebBrowser_News_C). The actual text is HTML rendered by
        // CEF, not exposed as UMG properties — we can only announce
        // that it's there and tell the player how to dismiss it.
        { L"WBP_WebBrowser_News_C",
          L"Patch notes. Press escape or B to close and continue to main menu." },

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

        // Quest / mission list panel inside the in-game pause menu.
        // WBP_QuestTab_C is the actual tab wrapper; its sub-widgets
        // (WBP_Quest_List_C / WBP_Quest_ForDisplay_C) are excluded from
        // the quest-menu catch-all below so we only announce once.
        { L"WBP_QuestTab_C",                    L"Mission list"            },
    };
    auto cls = Context->GetClassPrivate();
    if (!cls) return;
    auto name = cls->GetName();
    // Cache HUD gauge widgets so the F1/F2/F3 hotkeys can read their
    // displayed cur/max text even when SaveParameter's max is unpopulated.
    Hotkeys::NoticePotentialGaugeWidget(Context);

    // Recipe / craft widget discovery — log any class whose name
    // contains Recipe / Craft (not the ones we already know about)
    // the first time it constructs. Used to find the focusable widget
    // class at the workbench / cooking pot / etc.
    if (FunctionNameIs(Stack, L"Construct") &&
        (name.starts_with(L"WBP_") || name.starts_with(L"W_"))) {
        const bool looks_recipe =
            (name.find(L"Recipe") != std::wstring::npos ||
             name.find(L"Craft")  != std::wstring::npos ||
             name.find(L"Cook")   != std::wstring::npos) &&
            !name.ends_with(L"Button_C") &&
            !name.ends_with(L"Icon_C") &&
            !name.ends_with(L"_btn_C") &&
            !name.ends_with(L"Cursor_C");
        if (looks_recipe) {
            static std::mutex                       s_r_mtx;
            static std::unordered_set<std::wstring> s_r_seen;
            std::lock_guard<std::mutex> lock(s_r_mtx);
            if (s_r_seen.insert(name).second) {
                Output::send<LogLevel::Default>(
                    STR("[PalAccess] recipe/craft widget construct: {}\n"), name);
                // Also dump its properties so we can spot a GetTargetSlot
                // or similar method on it.
                auto* dcls = Context->GetClassPrivate();
                if (dcls) {
                    int n = 0;
                    for (auto* prop : dcls->ForEachPropertyInChain()) {
                        if (!prop) continue;
                        if (++n > 80) break;
                        auto ptype = prop->GetClass().GetName();
                        auto pname = prop->GetName();
                        if (ptype == L"ObjectProperty") {
                            auto** child = prop->ContainerPtrToValuePtr<Unreal::UObject*>(Context);
                            auto cn3 = (child && *child && (*child)->GetClassPrivate())
                                ? (*child)->GetClassPrivate()->GetName() : L"<null>";
                            Output::send<LogLevel::Default>(
                                STR("[PalAccess]   {} {} -> {}\n"), ptype, pname, cn3);
                        } else {
                            Output::send<LogLevel::Default>(
                                STR("[PalAccess]   {} {}\n"), ptype, pname);
                        }
                    }
                }
            }
        }
    }

    // Inventory + Pal slot discovery dumps. Each widget's properties
    // (and the class names of any ObjectProperty children) are logged
    // exactly once per class to UE4SS.log, so we can see in the user's
    // build where item / pal data is held. Pure logging — no spoken
    // output, no functional change. Remove once the property layout
    // for each is pinned in code.
    if (FunctionNameIs(Stack, L"Construct")) {
        struct DiscoveryTarget {
            std::wstring_view klass;
            bool              dive_into_children;  // also list each ObjectProperty child's class properties
        };
        static const DiscoveryTarget kTargets[] = {
            { L"WBP_PalInGameMenuItemSlotButton_C",   true  },
            { L"WBP_PalInGameMenuItemSlot_C",         true  },
            { L"WBP_PalCommonCharacterSlot_C",        true  },
            { L"WBP_PalCommonCharacterSlotButton_C",  true  },
            { L"WBP_InventoryEquipment_ItemInfo_C",   false },
            { L"WBP_Menu_PalList_C",                  false },
            { L"WBP_PalPlayerInventoryScrollList_C",  false },
        };
        for (const auto& target : kTargets) {
            if (name != target.klass) continue;
            static std::mutex                       s_disc_mtx;
            static std::unordered_set<std::wstring> s_disc_seen;
            {
                std::lock_guard<std::mutex> lock(s_disc_mtx);
                if (!s_disc_seen.insert(std::wstring(target.klass)).second) break;
            }
            Output::send<LogLevel::Default>(
                STR("[PalAccess] === discovery {} props ===\n"), target.klass);
            int n = 0;
            for (auto* prop : cls->ForEachPropertyInChain()) {
                if (!prop) continue;
                if (++n > 250) break;
                auto ptype = prop->GetClass().GetName();
                auto pname = prop->GetName();
                // For ObjectProperty, follow the pointer and report the
                // child's class name so we know what's bound to slots
                // (UPalItemSlot vs widget vs character handle, etc.).
                if (ptype == L"ObjectProperty") {
                    auto** child = prop->ContainerPtrToValuePtr<Unreal::UObject*>(Context);
                    Unreal::UObject* c = (child && *child) ? *child : nullptr;
                    auto cn = (c && c->GetClassPrivate())
                        ? c->GetClassPrivate()->GetName() : L"<null>";
                    Output::send<LogLevel::Default>(
                        STR("[PalAccess]   {} {} -> {}\n"), ptype, pname, cn);
                    if (target.dive_into_children && c && c->GetClassPrivate()) {
                        auto* sub_cls = c->GetClassPrivate();
                        int m = 0;
                        for (auto* sp : sub_cls->ForEachPropertyInChain()) {
                            if (!sp) continue;
                            if (++m > 80) {
                                Output::send<LogLevel::Default>(STR("[PalAccess]       ...\n"));
                                break;
                            }
                            Output::send<LogLevel::Default>(
                                STR("[PalAccess]       {} {}\n"),
                                sp->GetClass().GetName(), sp->GetName());
                        }
                    }
                } else {
                    Output::send<LogLevel::Default>(
                        STR("[PalAccess]   {} {}\n"), ptype, pname);
                }
            }
            break;
        }
    }

    // Cache the inner WebBrowserWidget for the patch-notes panel. Tick()
    // will poll GetUrl() on it until the URL becomes available, so we
    // can see where Palworld is fetching the notes from.
    if (FunctionNameIs(Stack, L"Construct") && name == L"WBP_WebBrowser_News_C") {
        for (auto* prop : cls->ForEachPropertyInChain()) {
            if (!prop) continue;
            if (prop->GetClass().GetName() != L"ObjectProperty") continue;
            if (prop->GetName() != L"WebBrowserWidget") continue;
            auto** child_ptr = prop->ContainerPtrToValuePtr<Unreal::UObject*>(Context);
            if (child_ptr && *child_ptr) {
                std::lock_guard<std::mutex> lock(g_news_mtx);
                g_news_inner   = *child_ptr;
                g_news_url_logged = false;
                g_news_construct_at = std::chrono::steady_clock::now();
            }
            break;
        }
    }

    // One-shot dump of the inner WebBrowserWidget that hosts the
    // patch-notes HTML — we want to find an InitialURL / Url string
    // we could fetch out-of-band and read out.
    if (FunctionNameIs(Stack, L"Construct") && name == L"WBP_WebBrowser_News_C") {
        static std::once_flag s_news_dumped;
        std::call_once(s_news_dumped, [&]{
            // Find the WebBrowserWidget ObjectProperty on the panel and
            // dump the inner widget's UClass properties.
            for (auto* prop : cls->ForEachPropertyInChain()) {
                if (!prop) continue;
                if (prop->GetClass().GetName() != L"ObjectProperty") continue;
                if (prop->GetName() != L"WebBrowserWidget") continue;
                auto** child_ptr = prop->ContainerPtrToValuePtr<Unreal::UObject*>(Context);
                if (!child_ptr || !*child_ptr) {
                    Output::send<LogLevel::Default>(
                        STR("[PalAccess] WebBrowserWidget pointer is null\n"));
                    return;
                }
                Unreal::UObject* inner = *child_ptr;
                auto* inner_cls = inner->GetClassPrivate();
                Output::send<LogLevel::Default>(
                    STR("[PalAccess] === WebBrowserWidget {} properties ===\n"),
                    inner_cls ? inner_cls->GetName() : L"<null cls>");
                if (!inner_cls) return;
                int n = 0;
                for (auto* p2 : inner_cls->ForEachPropertyInChain()) {
                    if (!p2) continue;
                    if (++n > 200) break;
                    Output::send<LogLevel::Default>(
                        STR("[PalAccess]   {} {}\n"),
                        p2->GetClass().GetName(), p2->GetName());
                    // For StrProperty fields, dump their current value too —
                    // that's where InitialURL would live.
                    if (p2->GetClass().GetName() == L"StrProperty") {
                        auto* str_ptr = p2->ContainerPtrToValuePtr<Unreal::FString>(inner);
                        if (str_ptr) {
                            const wchar_t* data = str_ptr->GetCharArray().GetData();
                            Output::send<LogLevel::Default>(
                                STR("[PalAccess]     -> \"{}\"\n"),
                                data ? data : L"");
                        }
                    }
                }
                return;
            }
            Output::send<LogLevel::Default>(
                STR("[PalAccess] WebBrowserWidget property not found on news panel\n"));
        });
    }

    for (const auto& e : kPanels) {
        if (name == e.klass) {
            // Two debounces:
            //   - Per-class (600 ms): Construct + OnSetup both pass our
            //     trigger filter, so the same panel fires twice. Drop
            //     the second.
            //   - Global cluster (500 ms): when entering Settings,
            //     Palworld constructs all five tab panels at once
            //     ("Graphics", "Audio", "Controls", "Key bindings",
            //     "Other") even though only one is visible. We want
            //     the user to hear the active tab — which is whichever
            //     one happens to fire first — and silence the rest.
            //     A real later tab-switch waits >500 ms, so it speaks.
            static std::mutex                                                                 s_arrival_mtx;
            static std::unordered_map<std::wstring, std::chrono::steady_clock::time_point>     s_arrival_at;
            static std::chrono::steady_clock::time_point                                       s_last_arrival_at{};
            const auto now = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(s_arrival_mtx);
                auto it = s_arrival_at.find(name);
                if (it != s_arrival_at.end()) {
                    const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - it->second).count();
                    if (since < 600) return;
                }
                if (s_last_arrival_at.time_since_epoch().count() != 0) {
                    const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - s_last_arrival_at).count();
                    if (since < 500) {
                        s_arrival_at[name] = now;
                        return;
                    }
                }
                s_arrival_at[name] = now;
                s_last_arrival_at = now;
            }
            Speech::Get().NotifyScreenArrival();
            Speech::Get().Announce(std::wstring_view(e.spoken));
            return;
        }
    }

    // Quest-list / quest-menu catch-all: when the player opens the
    // pause-menu quest tab, the panel that holds the list of active
    // missions constructs. We don't know its exact class name yet —
    // catch anything WBP_* that has "Quest" in the name and isn't a
    // child UI element we already handle (board / tracker / icon /
    // popup), log it, and announce "Quest list" so the player knows
    // they reached the screen.
    if (FunctionNameIs(Stack, L"Construct") &&
        (name.starts_with(L"WBP_") || name.starts_with(L"W_"))) {
        const bool looks_quest_menu =
            name.find(L"Quest") != std::wstring::npos &&
            !name.ends_with(L"Button_C") &&
            !name.ends_with(L"Icon_C") &&
            !name.ends_with(L"_btn_C") &&
            !name.ends_with(L"Slot_C") &&
            !name.ends_with(L"Cursor_C") &&
            !name.ends_with(L"Item_C") &&
            // Already handled / not the list menu:
            name != L"WBP_InGame_Quest_StartClear_C"        &&
            name != L"WBP_Ingame_QuestBoard_C"              &&
            name != L"WBP_QuestAndBaseCampInfoCanvas_C"     &&
            name != L"WBP_QuestTrackingIconCanvas_C"        &&
            name != L"WBP_PalQuestTrackingIcon_C"           &&
            name != L"WBP_IngameCompass_Quest_C"            &&
            name != L"WBP_NPC_OverheadQuest_C"              &&
            // Quest-tab sub-widgets — kPanels handles WBP_QuestTab_C
            // as the announce target; these are children that would
            // otherwise re-announce "Quest list" with placeholder text
            // (9999m before distance resolution, etc.).
            name != L"WBP_QuestTab_C"                       &&
            name != L"WBP_Quest_List_C"                     &&
            name != L"WBP_Quest_ForDisplay_C"               &&
            name.find(L"OverheadQuest") == std::wstring::npos &&
            name.find(L"TrackingIcon") == std::wstring::npos &&
            name.find(L"Compass")      == std::wstring::npos;
        if (looks_quest_menu) {
            static std::mutex                                                              s_q_mtx;
            static std::unordered_set<std::wstring>                                        s_q_seen;
            static std::unordered_map<std::wstring, std::chrono::steady_clock::time_point> s_q_at_class;
            static std::unordered_map<std::wstring, std::chrono::steady_clock::time_point> s_q_at_spoken;
            const auto now = std::chrono::steady_clock::now();
            const std::wstring spoken = L"Quest list";
            {
                std::lock_guard<std::mutex> lock(s_q_mtx);
                if (s_q_seen.insert(name).second) {
                    Output::send<LogLevel::Default>(
                        STR("[PalAccess] quest-menu construct: {}\n"), name);
                }
                auto it = s_q_at_class.find(name);
                if (it != s_q_at_class.end()) {
                    const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - it->second).count();
                    if (since < 600) return;
                }
                s_q_at_class[name] = now;
                auto sit = s_q_at_spoken.find(spoken);
                if (sit != s_q_at_spoken.end()) {
                    const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - sit->second).count();
                    if (since < 500) return;
                }
                s_q_at_spoken[spoken] = now;
            }
            Speech::Get().NotifyScreenArrival();
            Speech::Get().Announce(std::wstring_view(spoken));
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
            std::wstring spoken = L"Build menu";
            if (name.find(L"Placement") != std::wstring::npos) spoken = L"Placement menu";
            else if (name.find(L"Construction") != std::wstring::npos) spoken = L"Construction menu";
            // Same per-class + global cluster debounce as the kPanels
            // loop. Without it, opening the construction menu fires
            // Construct on several "Construction*" sub-widgets and the
            // user hears "Construction menu" repeated.
            static std::mutex                                                              s_b_mtx;
            static std::unordered_set<std::wstring>                                        s_b_seen;
            static std::unordered_map<std::wstring, std::chrono::steady_clock::time_point> s_b_at_class;
            static std::unordered_map<std::wstring, std::chrono::steady_clock::time_point> s_b_at_spoken;
            const auto now = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(s_b_mtx);
                if (s_b_seen.insert(name).second) {
                    Output::send<LogLevel::Default>(
                        STR("[PalAccess] build-shaped construct: {}\n"), name);
                }
                // Per-class: Construct + OnSetup pair on same widget.
                auto it = s_b_at_class.find(name);
                if (it != s_b_at_class.end()) {
                    const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - it->second).count();
                    if (since < 600) return;
                }
                s_b_at_class[name] = now;
                // Cluster on spoken text: any earlier announce of the
                // SAME phrase within 500 ms (different sub-widgets of
                // the construction menu) gets suppressed.
                auto sit = s_b_at_spoken.find(spoken);
                if (sit != s_b_at_spoken.end()) {
                    const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - sit->second).count();
                    if (since < 500) return;
                }
                s_b_at_spoken[spoken] = now;
            }
            Speech::Get().NotifyScreenArrival();
            Speech::Get().Announce(std::wstring_view(spoken));
            return;
        }
    }

    // Discovery aid: log the FIRST construct call for any class that LOOKS
    // panel-shaped (top-level WBP_*) so we can map new screens as they appear.
    // Limit to classes that don't look like a sub-widget (no nested suffixes).
    // Logged at Default level so the user can grep UE4SS.log to find the
    // class name of a new screen without enabling verbose logging.
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
            Output::send<LogLevel::Default>(
                STR("[PalAccess] panel Construct: {}\n"), name);
        }
    }
}

// A click happened. Strategy:
//  1) Cached SetText label (popup buttons) — silent. The next screen or
//     row update gives the real feedback.
//  2) Tagged-button label (title-menu / settings categories) — silent.
//     Same reasoning.
//  3) Settings-row click (changes a value): re-read the row via
//     reflection so the user hears the new state ("Mouse Sensitivity,
//     2"). FocusUpdate interrupts cleanly and plays the new value
//     tightly after the prior speech.
//  4) Unknown button — silent. "Selected button" is noise; if a click
//     produces no audible result we add a specific handler.
void HandleButtonClick(Unreal::UObject* Context, Unreal::FFrame& Stack) {
    if (!FunctionNameIs(Stack, L"BP_OnClicked")) return;

    // 1) Cached label — consume silently.
    if (auto cached = ButtonTextCache::Get().Lookup(Context); !cached.empty()) {
        return;
    }

    // 2) Tagged button — consume silently.
    if (auto tb = FindOutermostTaggedButton(Context); tb.widget) {
        return;
    }

    // 3) Settings row: re-extract label + new value, speak as new state.
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

    // 4) Unknown — silent.
}

// Designer-time placeholder fragments seen in Palworld widgets. If a
// SetText call carries any of these, it's the template default — not the
// real runtime message. Skip.
bool IsNotificationPlaceholder(std::wstring_view t) {
    if (t.empty()) return true;
    constexpr std::wstring_view kPlaceholders[] = {
        L"あいうえお", L"ABcdEFgh", L"ほげほげ", L"hogehoge",
        L"（仮）", L"Lorem ipsum", L"未完了は赤", L"パル名前",
        L"クエスト名",          // "quest name" — quest title placeholder
        L"クエスト",            // "quest" — generic fallback
        L"クエストID",
        L"ミッション名",        // "mission name"
        L"NPC名",
        L"前提未取得",          // "prerequisite not obtained" — tech tree locked label
        L"未習得",              // "not learned" — tech tree locked label
        L"未開放",              // "not unlocked"
        L"アイテム名",          // "item name"
        L"施設名",              // "facility name"
    };
    for (auto p : kPlaceholders) {
        if (t.find(p) != std::wstring::npos) return true;
    }
    // Designer-default distance placeholder used in quest list rows
    // before the real distance binds. Exactly the strings "9999m",
    // "9999 m", or the same with trailing characters.
    if (t == L"9999m" || t == L"9999 m") return true;
    return false;
}

// Cache the construction-menu info-panel widget so we can re-read it on
// every item-focus change (where Setup doesn't re-fire, only the inner
// TextBlocks get updated).
Unreal::UObject* g_construction_info_panel = nullptr;
std::mutex                                    g_construction_mtx;
std::wstring                                  g_construction_last;
std::chrono::steady_clock::time_point         g_construction_last_t{};

// Inventory: cache the alive WBP_InventoryEquipment_ItemInfo_C so slot
// focus can read its RichText_ItemName for the localized item name
// (Palworld populates it during the slot's hover handler — by the time
// our focus dispatcher fires, the text has resolved). Reset to nullptr
// on Destruct.
Unreal::UObject*                              g_inv_item_info = nullptr;
std::mutex                                    g_inv_mtx;
Unreal::UObject*                              g_inv_last_slot = nullptr;
std::chrono::steady_clock::time_point         g_inv_last_slot_t{};
// Name-based dedup: when an inventory has multiple slots of the same
// item (multiple stacks of Red Berries, Wood, etc.) each slot has its
// own widget pointer, so pointer dedup can't suppress the re-announce
// when the user arrows between them. Within this window the same
// computed message is skipped — slower browsing past adjacent same-
// item slots produces one announce, not N.
std::wstring                                  g_inv_last_msg;
std::chrono::steady_clock::time_point         g_inv_last_msg_t{};

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
// Convert a CamelCase / under_score class-id token into a spoken phrase.
// "WorkBench" -> "Work bench", "PalSphere" -> "Pal sphere",
// "Axe_Tier_00" -> "Axe tier", "RedBerries" -> "Red berries".
std::wstring HumanizeRichTextId(std::wstring_view id) {
    std::wstring out;
    out.reserve(id.size() + 4);
    for (size_t i = 0; i < id.size(); ++i) {
        wchar_t c = id[i];
        if (c == L'_') { out.push_back(L' '); continue; }
        if (i > 0 && c >= L'A' && c <= L'Z') {
            wchar_t prev = id[i - 1];
            bool prev_lower = prev >= L'a' && prev <= L'z';
            bool prev_digit = prev >= L'0' && prev <= L'9';
            if (prev_lower || prev_digit) out.push_back(L' ');
        }
        out.push_back(c);
    }
    // Trim trailing digits / spaces (Tier_00 -> "Tier ").
    while (!out.empty() &&
           (std::iswdigit(out.back()) || out.back() == L' '))
        out.pop_back();
    if (out.empty()) return {};
    // Lowercase everything after the first character so the output
    // reads naturally instead of "Work Bench Skill".
    out[0] = std::towupper(out[0]);
    for (size_t i = 1; i < out.size(); ++i) out[i] = std::towlower(out[i]);
    return out;
}

std::wstring StripRichTextMarkup(std::wstring_view s) {
    std::wstring tmp;
    tmp.reserve(s.size());
    // Two-pass approach: scan for tags, substitute the body text or
    // (when self-closing) the id="..." attribute humanized. Without
    // this, Palworld's tech-tree rows speak as empty after markup
    // strip because the only readable token is in id="WorkBench".
    size_t i = 0;
    while (i < s.size()) {
        wchar_t c = s[i];
        if (c != L'<') {
            if (c == L'\n' || c == L'\r' || c == L'\t') c = L' ';
            tmp.push_back(c);
            ++i;
            continue;
        }
        // Found a tag opening. Find its closing '>'.
        auto end = s.find(L'>', i + 1);
        if (end == std::wstring_view::npos) break;  // malformed; bail
        std::wstring_view tag_inner = s.substr(i + 1, end - i - 1);
        // If this tag has an id="..." attribute, capture it (used as
        // fallback when the tag is self-closing and has no body).
        std::wstring_view id_val;
        if (auto idp = tag_inner.find(L"id=\""); idp != std::wstring_view::npos) {
            idp += 4;
            auto endq = tag_inner.find(L'"', idp);
            if (endq != std::wstring_view::npos) {
                id_val = tag_inner.substr(idp, endq - idp);
            }
        }
        const bool self_closing =
            !tag_inner.empty() && tag_inner.back() == L'/';
        if (self_closing) {
            // Self-closing -- speak the id attribute, humanized.
            if (!id_val.empty()) {
                tmp += HumanizeRichTextId(id_val);
            }
            i = end + 1;
            continue;
        }
        // Open tag (e.g. <itemName id="X">Spear</itemName>). Skip to
        // the matching closing tag, keeping the body in the output.
        i = end + 1;  // step past the open tag
        // Body until '<' is what we want to preserve as-is.
        // The outer loop will handle the next tag (closing) and skip it.
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
    // Read Text_Category directly — that's the BP_PalTextBlock child
    // holding the category name (Production, Foundation, Defense, etc.)
    // Per the props dump on this class.
    std::wstring txt;
    auto* cls = Context->GetClassPrivate();
    if (cls) {
        for (auto* prop : cls->ForEachPropertyInChain()) {
            if (!prop) continue;
            if (prop->GetClass().GetName() != L"ObjectProperty") continue;
            if (prop->GetName() != L"Text_Category") continue;
            auto** ptr = prop->ContainerPtrToValuePtr<Unreal::UObject*>(Context);
            if (ptr && *ptr) txt = ReadChildTextOrLabel(*ptr);
            break;
        }
    }
    if (txt.empty()) {
        // Fallback: ExtractWidgetLabels in case Text_Category was
        // hidden or empty (different Palworld build / icon-only tab).
        txt = ExtractWidgetLabels(Context, 0);
    }
    if (txt.empty() || IsNotificationPlaceholder(txt)) return;
    if (txt.size() > 120) return;
    // Per-tab debounce — Palworld fires AnmEvent_Select more than once
    // for one logical tab change (animation + selection events).
    static std::mutex                                                              s_ctab_mtx;
    static std::unordered_map<std::wstring, std::chrono::steady_clock::time_point> s_ctab_at;
    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(s_ctab_mtx);
        auto it = s_ctab_at.find(txt);
        if (it != s_ctab_at.end()) {
            const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - it->second).count();
            if (since < 400) return;
        }
        s_ctab_at[txt] = now;
    }
    Output::send<LogLevel::Default>(
        STR("[PalAccess] construction tab: \"{}\"\n"), txt);
    Speech::Get().NotifyScreenArrival();
    Speech::Get().SpeakNow(std::wstring_view(txt));
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

// Recipe slot focus (crafting stations: workbench, primitive furnace,
// cooking pot, etc.). Mirror of the inventory-slot handler:
//   - WBP_PalConvertItemMenu_RecipeSlotButton_C::GetRecipeId is the
//     getter for the bound recipe.
//   - Return value is either an FName (recipe id) or a struct holding
//     one — we handle both via reflection.
//   - Humanize the raw id (e.g. "Recipe_WoodPlank" -> "Wood plank")
//     and announce via FocusUpdate.
void HandleRecipeSlotFocus(Unreal::UObject* Context, Unreal::FFrame& Stack) {
    if (!Context) return;
    auto cn = ClassNameOf(Context);
    auto fn = Stack.Node() ? Stack.Node()->GetName() : std::wstring();
    // Recipe slot widgets in this menu don't fire the standard
    // AnmEvent_Focus / BP_OnHovered. Palworld signals "this slot is
    // being inspected" by calling GetRecipeId on the slot widget and
    // by firing OnHoveredRecipeSlot on the workspace container. Use
    // either as the trigger.
    const bool is_recipe_trigger =
        (cn == L"WBP_PalConvertItemMenu_RecipeSlotButton_C" &&
         fn == L"GetRecipeId") ||
        (cn == L"WBP_IngameMenu_WorkSpace_C" &&
         fn == L"OnHoveredRecipeSlot");
    if (!is_recipe_trigger) return;
    // For the workspace OnHoveredRecipeSlot path Context isn't the
    // slot widget itself; we need to read the slot reference from
    // its parameter. For now only act when Context IS the slot widget
    // (the GetRecipeId path) — that's the common case.
    if (cn != L"WBP_PalConvertItemMenu_RecipeSlotButton_C") return;

    // Pointer dedup (~800 ms): Palworld's click flow re-invokes
    // GetRecipeId several times on the same slot in quick succession,
    // and the announcement chain below interrupts itself mid-word each
    // time. The wider window catches those bursts without making
    // legitimate D-pad navigation feel laggy.
    static std::mutex                                   s_mtx;
    static Unreal::UObject*                             s_last_slot = nullptr;
    static std::chrono::steady_clock::time_point        s_last_slot_t;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(s_mtx);
        if (Context == s_last_slot &&
            now - s_last_slot_t < std::chrono::milliseconds(800)) return;
        // User moved to a different slot — clear the click handler's
        // "already announced" flag so a click on the new slot fires,
        // and a future return to a previously-announced slot fires
        // again as a fresh visit.
        if (Context != s_last_slot) g_click_announced_slot = nullptr;
        s_last_slot   = Context;
        s_last_slot_t = now;
    }

    auto* func = FindFunctionInChain(Context, L"GetRecipeId");
    if (!func) return;
    const size_t ps = func->GetPropertiesSize();
    if (ps == 0 || ps > 256) return;
    std::vector<uint8_t> buf(ps, 0);
    Context->ProcessEvent(func, buf.data());

    // The return parameter is either a NameProperty or a StructProperty
    // wrapping a NameProperty (FPalRecipeId or similar). Try both.
    std::wstring raw;
    // GetRecipeId returns a single FName (or a struct wrapping one).
    // First try the StructProperty case (same shape as GetItemId).
    for (auto* prop : func->ForEachProperty()) {
        if (!prop) continue;
        if (prop->GetClass().GetName() != L"StructProperty") continue;
        auto* sp = reinterpret_cast<Unreal::FStructProperty*>(prop);
        auto* inner = sp->GetStruct().Get();
        if (!inner) continue;
        auto* struct_data = prop->ContainerPtrToValuePtr<uint8_t>(buf.data());
        if (!struct_data) continue;
        for (auto* leaf : inner->ForEachProperty()) {
            if (!leaf) continue;
            if (leaf->GetClass().GetName() != L"NameProperty") continue;
            auto* fname = leaf->ContainerPtrToValuePtr<Unreal::FName>(struct_data);
            if (fname) {
                auto s = std::wstring(fname->ToString());
                if (!s.empty() && s != L"None") { raw = std::move(s); break; }
            }
        }
        if (!raw.empty()) break;
    }
    // Fallback: top-level NameProperty return value.
    if (raw.empty()) {
        for (auto* prop : func->ForEachProperty()) {
            if (!prop) continue;
            if (prop->GetClass().GetName() != L"NameProperty") continue;
            auto* fname = prop->ContainerPtrToValuePtr<Unreal::FName>(buf.data());
            if (fname) {
                auto s = std::wstring(fname->ToString());
                if (!s.empty() && s != L"None") { raw = std::move(s); break; }
            }
        }
    }
    if (raw.empty()) return;

    // Try the live WBP_PalCraftInfo_C info panel's Text_ItemName for
    // the localized display name. Palworld writes the resolved name
    // there on each hover, regardless of how the FName maps. Falls
    // back to the humanized raw FName when the panel can't be read.
    std::wstring resolved;
    Unreal::UObject* info_panel = nullptr;
    Unreal::UObjectGlobals::ForEachUObject(
        [&](Unreal::UObject* obj, int32_t, int32_t) -> LoopAction {
            if (info_panel || !obj) return LoopAction::Continue;
            if (obj->GetName().starts_with(L"Default__")) return LoopAction::Continue;
            auto* ocls = obj->GetClassPrivate();
            if (ocls && ocls->GetName() == L"WBP_PalCraftInfo_C") {
                info_panel = obj;
                return LoopAction::Break;
            }
            return LoopAction::Continue;
        });
    if (info_panel) {
        auto* pcls = info_panel->GetClassPrivate();
        if (pcls) {
            for (auto* prop : pcls->ForEachPropertyInChain()) {
                if (!prop) continue;
                if (prop->GetClass().GetName() != L"ObjectProperty") continue;
                if (prop->GetName() != L"Text_ItemName") continue;
                auto** ptr = prop->ContainerPtrToValuePtr<Unreal::UObject*>(info_panel);
                if (!ptr || !*ptr) break;
                Unreal::UObject* tb = *ptr;
                auto* tbc = tb->GetClassPrivate();
                if (!tbc) break;
                for (auto* tp : tbc->ForEachPropertyInChain()) {
                    if (!tp) continue;
                    if (tp->GetClass().GetName() != L"TextProperty") continue;
                    if (tp->GetName() != L"Text") continue;
                    auto* ftext = tp->ContainerPtrToValuePtr<Unreal::FText>(tb);
                    if (ftext) {
                        auto s = std::wstring(ftext->ToString());
                        if (!s.empty() && !IsPlaceholderText(s) &&
                            !IsNotificationPlaceholder(s)) {
                            resolved = StripRichTextMarkup(s);
                        }
                    }
                    break;
                }
                break;
            }
        }
    }

    auto name = resolved.empty() ? HumanizeRichTextId(raw) : resolved;
    Output::send<LogLevel::Default>(
        STR("[PalAccess] recipe-focus raw=\"{}\" -> \"{}\"\n"), raw, name);
    // Message dedup against the shared recipe-speech state — same name
    // within 2.5 s is suppressed regardless of whether it was the hover
    // or the click handler that last spoke it. The click flow announces
    // "<name>. Requires …" which starts with the name, so a subsequent
    // hover with just the name would be redundant.
    {
        std::lock_guard<std::mutex> lock(g_recipe_speech_mtx);
        if (now - g_recipe_last_msg_t < std::chrono::milliseconds(2500) &&
            (name == g_recipe_last_msg ||
             g_recipe_last_msg.starts_with(name))) return;
        g_recipe_last_msg   = name;
        g_recipe_last_msg_t = now;
    }
    Speech::Get().FocusUpdate(std::wstring_view(name));
    // Cache the currently-hovered recipe slot so the click handler
    // below can read its MatMap when the player activates it.
    g_last_recipe_slot   = Context;
    g_last_recipe_slot_t = now;
}

// Resolve a recipe slot's display name. Mirrors the logic used in
// HandleRecipeSlotFocus: pull the raw FName via GetRecipeId, then look
// up the live WBP_PalCraftInfo_C panel's Text_ItemName for the localized
// player-facing name. Falls back to humanized raw FName if the panel
// can't be read.
std::wstring ResolveRecipeDisplayName(Unreal::UObject* slot) {
    if (!slot) return {};
    std::wstring raw;
    auto* func = FindFunctionInChain(slot, L"GetRecipeId");
    if (func) {
        const size_t ps = func->GetPropertiesSize();
        if (ps > 0 && ps <= 256) {
            std::vector<uint8_t> buf(ps, 0);
            slot->ProcessEvent(func, buf.data());
            for (auto* prop : func->ForEachProperty()) {
                if (!prop) continue;
                if (prop->GetClass().GetName() == L"StructProperty") {
                    auto* sp = reinterpret_cast<Unreal::FStructProperty*>(prop);
                    auto* inner = sp->GetStruct().Get();
                    if (!inner) continue;
                    auto* sd = prop->ContainerPtrToValuePtr<uint8_t>(buf.data());
                    if (!sd) continue;
                    for (auto* leaf : inner->ForEachProperty()) {
                        if (!leaf) continue;
                        if (leaf->GetClass().GetName() != L"NameProperty") continue;
                        auto* fname =
                            leaf->ContainerPtrToValuePtr<Unreal::FName>(sd);
                        if (fname) {
                            auto s = std::wstring(fname->ToString());
                            if (!s.empty() && s != L"None") {
                                raw = std::move(s);
                                break;
                            }
                        }
                    }
                    if (!raw.empty()) break;
                } else if (prop->GetClass().GetName() == L"NameProperty") {
                    auto* fname =
                        prop->ContainerPtrToValuePtr<Unreal::FName>(buf.data());
                    if (fname) {
                        auto s = std::wstring(fname->ToString());
                        if (!s.empty() && s != L"None") {
                            raw = std::move(s);
                            break;
                        }
                    }
                }
            }
        }
    }

    std::wstring resolved;
    Unreal::UObject* info_panel = nullptr;
    Unreal::UObjectGlobals::ForEachUObject(
        [&](Unreal::UObject* obj, int32_t, int32_t) -> LoopAction {
            if (info_panel || !obj) return LoopAction::Continue;
            if (obj->GetName().starts_with(L"Default__")) return LoopAction::Continue;
            auto* ocls = obj->GetClassPrivate();
            if (ocls && ocls->GetName() == L"WBP_PalCraftInfo_C") {
                info_panel = obj;
                return LoopAction::Break;
            }
            return LoopAction::Continue;
        });
    if (info_panel) {
        auto* pcls = info_panel->GetClassPrivate();
        if (pcls) {
            for (auto* prop : pcls->ForEachPropertyInChain()) {
                if (!prop) continue;
                if (prop->GetClass().GetName() != L"ObjectProperty") continue;
                if (prop->GetName() != L"Text_ItemName") continue;
                auto** ptr =
                    prop->ContainerPtrToValuePtr<Unreal::UObject*>(info_panel);
                if (!ptr || !*ptr) break;
                Unreal::UObject* tb = *ptr;
                auto* tbc = tb->GetClassPrivate();
                if (!tbc) break;
                for (auto* tp : tbc->ForEachPropertyInChain()) {
                    if (!tp) continue;
                    if (tp->GetClass().GetName() != L"TextProperty") continue;
                    if (tp->GetName() != L"Text") continue;
                    auto* ftext = tp->ContainerPtrToValuePtr<Unreal::FText>(tb);
                    if (ftext) {
                        auto s = std::wstring(ftext->ToString());
                        if (!s.empty() && !IsPlaceholderText(s) &&
                            !IsNotificationPlaceholder(s)) {
                            resolved = StripRichTextMarkup(s);
                        }
                    }
                    break;
                }
                break;
            }
        }
    }

    if (!resolved.empty()) return resolved;
    if (!raw.empty()) return HumanizeRichTextId(raw);
    return {};
}

// Read the materials map (MatMap) off a recipe slot button via reflection.
// MatMap is a TMap<FPalItemId, int32> (or TMap<FName, int32> in some
// variants): key = ingredient item id, value = required count.
struct RecipeMaterial { std::wstring name; int32_t count; };

std::vector<RecipeMaterial> ReadRecipeMaterials(Unreal::UObject* slot) {
    std::vector<RecipeMaterial> out;
    if (!slot) return out;
    auto* cls = slot->GetClassPrivate();
    if (!cls) return out;
    for (auto* prop : cls->ForEachPropertyInChain()) {
        if (!prop) continue;
        if (prop->GetClass().GetName() != L"MapProperty") continue;
        if (prop->GetName() != L"MatMap") continue;
        auto* map_prop = static_cast<Unreal::FMapProperty*>(prop);
        auto* key_prop = map_prop->GetKeyProp();
        auto* val_prop = map_prop->GetValueProp();
        if (!key_prop || !val_prop) break;
        const auto& layout = map_prop->GetMapLayout();
        auto* sm = map_prop->ContainerPtrToValuePtr<Unreal::FScriptMap>(slot);
        if (!sm) break;
        const int32_t max_index = sm->GetMaxIndex();
        const auto key_cls = key_prop->GetClass().GetName();
        const auto val_cls = val_prop->GetClass().GetName();
        for (int32_t i = 0; i < max_index; ++i) {
            if (!sm->IsValidIndex(i)) continue;
            uint8_t* pair = static_cast<uint8_t*>(sm->GetData(i, layout));
            if (!pair) continue;

            std::wstring key_name;
            if (key_cls == L"NameProperty") {
                auto* fname = reinterpret_cast<Unreal::FName*>(pair);
                if (fname) {
                    auto s = std::wstring(fname->ToString());
                    if (!s.empty() && s != L"None") key_name = s;
                }
            } else if (key_cls == L"StructProperty") {
                auto* sp = static_cast<Unreal::FStructProperty*>(key_prop);
                auto* inner = sp->GetStruct().Get();
                if (inner) {
                    for (auto* leaf : inner->ForEachProperty()) {
                        if (!leaf) continue;
                        if (leaf->GetClass().GetName() != L"NameProperty") continue;
                        auto* fname = reinterpret_cast<Unreal::FName*>(
                            pair + leaf->GetOffset_Internal());
                        if (fname) {
                            auto s = std::wstring(fname->ToString());
                            if (!s.empty() && s != L"None") {
                                key_name = s;
                                break;
                            }
                        }
                    }
                }
            }

            int32_t count = 0;
            if (val_cls == L"IntProperty") {
                count = *reinterpret_cast<int32_t*>(pair + layout.ValueOffset);
            } else if (val_cls == L"Int64Property") {
                count = static_cast<int32_t>(
                    *reinterpret_cast<int64_t*>(pair + layout.ValueOffset));
            }

            if (!key_name.empty()) {
                out.push_back({HumanizeRichTextId(key_name), count});
            }
        }
        break;
    }
    return out;
}

// Recipe slot click → announce required materials. We don't know the exact
// UFunction Palworld fires on confirm, so we accept any function whose
// name contains "click" / "decide" / "press" / "confirm" / "submit" /
// "activate" (case-insensitive) and falls outside the hover-probe set.
// Any unmatched call is logged so we can refine if needed.
void HandleRecipeSlotClick(Unreal::UObject* Context, Unreal::FFrame& Stack) {
    if (!Context) return;
    if (ClassNameOf(Context) != L"WBP_PalConvertItemMenu_RecipeSlotButton_C") return;
    auto fn = Stack.Node() ? Stack.Node()->GetName() : std::wstring();
    if (fn.empty()) return;

    // Skip the hover/probe events handled elsewhere.
    if (fn == L"GetRecipeId" ||
        fn == L"BP_OnHovered" ||
        fn == L"AnmEvent_Focus" ||
        fn == L"AnmEvent_Hover") return;

    std::wstring fn_lower = fn;
    for (auto& c : fn_lower) c = static_cast<wchar_t>(std::towlower(c));
    const bool is_click =
        fn_lower.find(L"click")    != std::wstring::npos ||
        fn_lower.find(L"decide")   != std::wstring::npos ||
        fn_lower.find(L"press")    != std::wstring::npos ||
        fn_lower.find(L"confirm")  != std::wstring::npos ||
        fn_lower.find(L"submit")   != std::wstring::npos ||
        fn_lower.find(L"activate") != std::wstring::npos;

    if (!is_click) {
        // Discovery log — quiet, fires only on the slot button.
        Output::send<LogLevel::Default>(
            STR("[PalAccess] recipe-slot ufunc (unmatched): {}\n"), fn);
        return;
    }

    // Per-visit dedup: only announce the materials line once per stay on
    // a slot. CommonButtonBaseClicked fires repeatedly while the slot
    // stays focused (multiple bound events + Palworld's polling); the
    // hover handler clears g_click_announced_slot when the user moves
    // to a different slot, so revisiting re-announces.
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_recipe_speech_mtx);
        if (Context == g_click_announced_slot) return;
        g_click_announced_slot = Context;
    }

    auto mats = ReadRecipeMaterials(Context);
    auto recipe_name = ResolveRecipeDisplayName(Context);
    Output::send<LogLevel::Default>(
        STR("[PalAccess] recipe-click ufunc=\"{}\" name=\"{}\" mats={}\n"),
        fn, recipe_name, mats.size());

    std::wstring msg;
    if (!recipe_name.empty()) {
        msg = recipe_name;
        msg += L". ";
    }
    if (mats.empty()) {
        msg += L"No required materials";
    } else {
        msg += L"Requires ";
        for (size_t i = 0; i < mats.size(); ++i) {
            if (i) msg += (i + 1 == mats.size()) ? L" and " : L", ";
            msg += std::to_wstring(mats[i].count);
            msg += L" ";
            msg += mats[i].name;
        }
    }
    // Mark the full click message as last-spoken so the hover handler's
    // dedup recognises a same-prefix announce ("<name>") as redundant.
    {
        std::lock_guard<std::mutex> lock(g_recipe_speech_mtx);
        g_recipe_last_msg   = msg;
        g_recipe_last_msg_t = now;
    }
    Speech::Get().FocusUpdate(std::wstring_view(msg));
}

// Pause-menu tab change. WBP_InGameMainMenu_C fires SelectXxxTab when
// the active tab changes (via RB/LB or click). Announce the new tab so
// the user immediately knows where they are without having to wait for
// the widget construction cascade.
void HandleInGameMenuTabSelect(Unreal::UObject* Context, Unreal::FFrame& Stack) {
    if (!Context) return;
    if (ClassNameOf(Context) != L"WBP_InGameMainMenu_C") return;
    auto fn = Stack.Node() ? Stack.Node()->GetName() : std::wstring();
    std::wstring_view spoken;
    if      (fn == L"SelectInventoryEquipmentTab") spoken = L"Inventory tab";
    else if (fn == L"SelectPalTab")                spoken = L"Pal tab";
    else if (fn == L"SelectTechnologyTab")         spoken = L"Technology tab";
    else if (fn == L"SelectQuestTab")              spoken = L"Mission list tab";
    else if (fn == L"SelectMapTab")                spoken = L"Map tab";
    else if (fn == L"SelectStatusTab")             spoken = L"Status tab";
    else return;
    // Per-tab debounce: Palworld may fire Select* multiple times in
    // quick succession when navigating (e.g. once for input, once for
    // widget setup). 400 ms catches the burst.
    static std::mutex                                                              s_tab_mtx;
    static std::unordered_map<std::wstring, std::chrono::steady_clock::time_point> s_tab_at;
    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(s_tab_mtx);
        auto it = s_tab_at.find(std::wstring(spoken));
        if (it != s_tab_at.end()) {
            const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - it->second).count();
            if (since < 400) return;
        }
        s_tab_at[std::wstring(spoken)] = now;
    }
    Speech::Get().NotifyScreenArrival();
    Speech::Get().SpeakNow(spoken);
}

// ---- Inventory slot accessibility ----------------------------------------
//
// Two coordinated handlers:
//   HandleInventoryInfoLifecycle — caches the alive
//     WBP_InventoryEquipment_ItemInfo_C panel pointer so slot focus can
//     read its RichText_ItemName for the resolved localized item name.
//     Cleared on Destruct.
//   HandleInventorySlotFocus — fires on hover/focus events on
//     WBP_PalInGameMenuItemSlotButton_C. Reads IsUsableSlot for empty
//     detection, the slot's own count text-block, and the cached info
//     panel's name field. Announces "<item name>, <count>" via
//     FocusUpdate so it interrupts the previous label for snappy nav.
//     If the slot is empty, just "Empty slot".

// Read the item name text from the cached WBP_InventoryEquipment_ItemInfo_C
// panel (if it's alive). Walks the panel's properties for the
// RichText_ItemName ObjectProperty, reads that child's Text field.
std::wstring ReadInventoryItemName(Unreal::UObject* panel) {
    if (!panel) return {};
    auto* cls = panel->GetClassPrivate();
    if (!cls) return {};
    for (auto* prop : cls->ForEachPropertyInChain()) {
        if (!prop) continue;
        if (prop->GetClass().GetName() != L"ObjectProperty") continue;
        if (prop->GetName() != L"RichText_ItemName") continue;
        auto** ptr = prop->ContainerPtrToValuePtr<Unreal::UObject*>(panel);
        if (!ptr || !*ptr) return {};
        // Bypass the IsWidgetVisibleForReading gate that
        // ReadChildTextOrLabel applies — Palworld's RichText reports
        // Collapsed/Hidden visibility on transient updates even while
        // it's actually rendering. Read the FText "Text" property
        // directly.
        Unreal::UObject* child = *ptr;
        auto* child_cls = child->GetClassPrivate();
        if (!child_cls) return {};
        for (auto* tp : child_cls->ForEachPropertyInChain()) {
            if (!tp) continue;
            if (tp->GetClass().GetName() != L"TextProperty") continue;
            if (tp->GetName() != L"Text") continue;
            auto* ftext = tp->ContainerPtrToValuePtr<Unreal::FText>(child);
            if (!ftext) return {};
            auto raw = std::wstring(ftext->ToString());
            Output::send<LogLevel::Default>(
                STR("[PalAccess] inv-name raw RichText_ItemName.Text=\"{}\"\n"),
                raw);
            return StripRichTextMarkup(raw);
        }
        return {};
    }
    return {};
}

// Call the slot button's GetItemAndNum() UFunction to read the live
// FPalItemAndNum (the bound slot data). The widget's own Item_Info
// member is a stale default; only the getter returns the current
// bound item. Confirmed struct layout from localcc/PalworldModdingKit:
//   struct FPalItemAndNum {
//       FPalItemId ItemId;   // { FName StaticId; FPalDynamicItemId DynamicId; }
//       int32      Num;
//   };
// The UFunction's only parameter is the out struct ItemAndNum. We
// allocate a zero-filled buffer of the function's parameter size,
// ProcessEvent into it, then walk the buffer via reflection to pull
// ItemId.StaticId (FName) and Num.
std::wstring ReadSlotItemViaGetter(Unreal::UObject* slot_button, int32_t* out_count) {
    if (out_count) *out_count = 0;
    if (!slot_button) return {};
    auto* func = slot_button->GetFunctionByName(L"GetItemAndNum");
    if (!func) return {};
    const size_t param_size = func->GetPropertiesSize();
    if (param_size == 0 || param_size > 1024) return {};
    std::vector<uint8_t> buffer(param_size, 0);
    slot_button->ProcessEvent(func, buffer.data());
    // The function has exactly one parameter — the StructProperty
    // ItemAndNum (FPalItemAndNum). Find it, walk into its ItemId
    // sub-struct, take StaticId. Also pull Num out.
    std::wstring name;
    for (auto* prop : func->ForEachProperty()) {
        if (!prop) continue;
        if (prop->GetClass().GetName() != L"StructProperty") continue;
        auto* sp = reinterpret_cast<Unreal::FStructProperty*>(prop);
        auto* inner = sp->GetStruct().Get();
        if (!inner) continue;
        auto* struct_data = prop->ContainerPtrToValuePtr<uint8_t>(buffer.data());
        for (auto* sub : inner->ForEachProperty()) {
            if (!sub) continue;
            auto stype = sub->GetClass().GetName();
            auto sname = sub->GetName();
            if (stype == L"IntProperty" && sname == L"Num") {
                auto* num_ptr = sub->ContainerPtrToValuePtr<int32_t>(struct_data);
                if (num_ptr && out_count) *out_count = *num_ptr;
                continue;
            }
            if (stype == L"StructProperty" && sname == L"ItemId") {
                auto* sub_sp = reinterpret_cast<Unreal::FStructProperty*>(sub);
                auto* sub_inner = sub_sp->GetStruct().Get();
                if (!sub_inner) continue;
                auto* sub_data = sub->ContainerPtrToValuePtr<uint8_t>(struct_data);
                if (!sub_data) continue;
                for (auto* leaf : sub_inner->ForEachProperty()) {
                    if (!leaf) continue;
                    if (leaf->GetClass().GetName() != L"NameProperty") continue;
                    if (leaf->GetName() != L"StaticId") continue;
                    auto* fname = leaf->ContainerPtrToValuePtr<Unreal::FName>(sub_data);
                    if (fname) {
                        auto s = std::wstring(fname->ToString());
                        if (!s.empty() && s != L"None") name = std::move(s);
                    }
                    break;
                }
            }
        }
        break;
    }
    return name;
}

// Legacy struct-read fallback (kept as backup).
std::wstring ReadSlotItemFromStruct(Unreal::UObject* slot_button, int32_t* out_count) {
    if (out_count) *out_count = 0;
    if (!slot_button) return {};
    auto* cls = slot_button->GetClassPrivate();
    if (!cls) return {};
    for (auto* prop : cls->ForEachPropertyInChain()) {
        if (!prop) continue;
        if (prop->GetClass().GetName() != L"StructProperty") continue;
        if (prop->GetName() != L"Item Info") continue;
        auto* sp = reinterpret_cast<Unreal::FStructProperty*>(prop);
        auto* inner = sp->GetStruct().Get();
        if (!inner) break;
        auto* struct_data = prop->ContainerPtrToValuePtr<uint8_t>(slot_button);
        if (!struct_data) break;

        std::wstring item_name;
        for (auto* sub : inner->ForEachProperty()) {
            if (!sub) continue;
            auto stype = sub->GetClass().GetName();
            auto sname = sub->GetName();
            if (stype == L"IntProperty" && sname == L"Num") {
                auto* num_ptr = sub->ContainerPtrToValuePtr<int32_t>(struct_data);
                if (num_ptr && out_count) *out_count = *num_ptr;
                continue;
            }
            if (stype == L"StructProperty" && sname == L"ItemId") {
                auto* sub_sp = reinterpret_cast<Unreal::FStructProperty*>(sub);
                auto* sub_inner = sub_sp->GetStruct().Get();
                if (!sub_inner) continue;
                auto* sub_data = sub->ContainerPtrToValuePtr<uint8_t>(struct_data);
                if (!sub_data) continue;
                // First-time discovery: log the ItemId struct's inner
                // fields so we can refine if needed.
                static std::once_flag s_id_dump;
                std::call_once(s_id_dump, [&]{
                    Output::send<LogLevel::Default>(
                        STR("[PalAccess] === ItemId struct ({}) fields ===\n"),
                        sub_inner->GetName());
                    int m = 0;
                    for (auto* leaf : sub_inner->ForEachProperty()) {
                        if (!leaf) continue;
                        if (++m > 40) break;
                        Output::send<LogLevel::Default>(
                            STR("[PalAccess]   {} {}\n"),
                            leaf->GetClass().GetName(), leaf->GetName());
                    }
                });
                // Grab the first non-empty NameProperty inside.
                for (auto* leaf : sub_inner->ForEachProperty()) {
                    if (!leaf) continue;
                    if (leaf->GetClass().GetName() != L"NameProperty") continue;
                    auto* fname = leaf->ContainerPtrToValuePtr<Unreal::FName>(sub_data);
                    if (fname && fname->ToString() != L"None" && !fname->ToString().empty()) {
                        item_name = std::wstring(fname->ToString());
                        break;
                    }
                }
            }
        }
        return item_name;
    }
    return {};
}

// Read the visible stack-count text. Tries multiple sources in order:
//   1. The slot button's BP_PalTextBlock_NumRange child (the in-grid
//      count overlay; Palworld hides this for single-quantity items)
//   2. The slot button's "Override Num Count Text" TextProperty
//      (overlay override; usually empty unless Palworld set it)
//   3. The ItemInfo panel's Text_ItemNumValue child (the right-pane
//      detailed count, always resolved on hover)
// First non-empty result wins.
std::wstring ReadInventorySlotCount(Unreal::UObject* slot_button,
                                    Unreal::UObject* info_panel) {
    auto read_object_child_text = [](Unreal::UObject* owner,
                                     std::wstring_view child_name) -> std::wstring {
        if (!owner) return {};
        auto* cls = owner->GetClassPrivate();
        if (!cls) return {};
        for (auto* prop : cls->ForEachPropertyInChain()) {
            if (!prop) continue;
            if (prop->GetClass().GetName() != L"ObjectProperty") continue;
            if (prop->GetName() != child_name) continue;
            auto** ptr = prop->ContainerPtrToValuePtr<Unreal::UObject*>(owner);
            if (!ptr || !*ptr) return {};
            return ReadChildTextOrLabel(*ptr);
        }
        return {};
    };

    if (slot_button) {
        if (auto t = read_object_child_text(slot_button, L"BP_PalTextBlock_NumRange");
            !t.empty()) return t;
        // Override Num Count Text — TextProperty directly on the slot button.
        auto* cls = slot_button->GetClassPrivate();
        if (cls) {
            for (auto* prop : cls->ForEachPropertyInChain()) {
                if (!prop) continue;
                if (prop->GetClass().GetName() != L"TextProperty") continue;
                if (prop->GetName() != L"Override Num Count Text") continue;
                auto* ftext = prop->ContainerPtrToValuePtr<Unreal::FText>(slot_button);
                if (ftext) {
                    auto s = std::wstring(ftext->ToString());
                    if (!s.empty()) return s;
                }
                break;
            }
        }
    }
    if (info_panel) {
        if (auto t = read_object_child_text(info_panel, L"Text_ItemNumValue");
            !t.empty()) return t;
    }
    return {};
}

// Read the IsUsableSlot bool property on the slot button. True when the
// slot holds an item that can be picked / equipped. Returns false on
// any failure path (we'd rather not announce an item name for an empty
// slot than vice-versa).
bool SlotIsUsable(Unreal::UObject* slot_button) {
    if (!slot_button) return false;
    auto* cls = slot_button->GetClassPrivate();
    if (!cls) return false;
    for (auto* prop : cls->ForEachPropertyInChain()) {
        if (!prop) continue;
        if (prop->GetClass().GetName() != L"BoolProperty") continue;
        if (prop->GetName() != L"IsUsableSlot") continue;
        auto* val = prop->ContainerPtrToValuePtr<bool>(slot_button);
        return val && *val;
    }
    return false;
}

// Call WBP_PalInGameMenuItemSlotButton_C::GetTargetSlot via ProcessEvent
// to retrieve the bound UPalItemSlot. The slot widget's own Item_Info
// member is a stale display copy (especially on the Equipment subtab
// where it never gets filled); GetTargetSlot returns the live data
// source, which we then ask for its ItemId.
//
// Confirmed flow from log + reverse-engineering research:
//   slot_button.GetTargetSlot() -> UPalItemSlot*
//   UPalItemSlot.GetItemId()    -> FPalItemId { FName StaticId; ... }
//   UPalItemSlot.GetStackCount() -> int32
// Walk the class chain looking for a UFunction by name. Built-in
// GetFunctionByName returned null for GetTargetSlot in this Palworld
// build even though the function exists on the parent class
// WBP_PalItemSlotButtonBase_C (confirmed via class-chain dump). This
// iterator is paranoid and walks until we hit base UObject/Widget.
Unreal::UFunction* FindFunctionInChain(Unreal::UObject* obj,
                                       std::wstring_view fname) {
    if (!obj) return nullptr;
    auto* cls = obj->GetClassPrivate();
    while (cls) {
        for (auto* f : cls->ForEachFunction()) {
            if (f && f->GetName() == fname) return f;
        }
        auto* super = cls->GetSuperStruct();
        cls = reinterpret_cast<Unreal::UClass*>(super);
        if (!cls) break;
        auto cn = cls->GetName();
        if (cn == L"UserWidget" || cn == L"CommonUserWidget" ||
            cn == L"Widget"     || cn == L"Object") break;
    }
    return nullptr;
}

Unreal::UObject* CallGetTargetSlot(Unreal::UObject* slot_button) {
    static std::once_flag s_step_log;
    std::call_once(s_step_log, [&]{
        Output::send<LogLevel::Default>(
            STR("[PalAccess] CallGetTargetSlot first call slot={:p}\n"),
            static_cast<void*>(slot_button));
        // Dump every UFunction we can find on the slot button's class
        // chain so we can see the exact name + which class owns it.
        if (auto* cls = slot_button->GetClassPrivate()) {
            Output::send<LogLevel::Default>(
                STR("[PalAccess] slot class chain functions:\n"));
            for (auto* uc = cls; uc; ) {
                Output::send<LogLevel::Default>(
                    STR("[PalAccess]   class {}\n"), uc->GetName());
                for (auto* f : uc->ForEachFunction()) {
                    if (!f) continue;
                    Output::send<LogLevel::Default>(
                        STR("[PalAccess]     fn {}\n"), f->GetName());
                }
                // Walk up the parent chain.
                auto* super = uc->GetSuperStruct();
                uc = reinterpret_cast<Unreal::UClass*>(super);
                if (!uc) break;
                if (uc->GetName() == L"UserWidget" ||
                    uc->GetName() == L"CommonUserWidget" ||
                    uc->GetName() == L"Widget" ||
                    uc->GetName() == L"Object") break;
            }
        }
    });
    if (!slot_button) return nullptr;
    auto* func = FindFunctionInChain(slot_button, L"GetTargetSlot");
    static std::once_flag s_func_log;
    std::call_once(s_func_log, [&]{
        Output::send<LogLevel::Default>(
            STR("[PalAccess] CallGetTargetSlot func={:p} (chain-walked)\n"),
            static_cast<void*>(func));
    });
    if (!func) return nullptr;
    const size_t param_size = func->GetPropertiesSize();
    static std::once_flag s_size_log;
    std::call_once(s_size_log, [&]{
        Output::send<LogLevel::Default>(
            STR("[PalAccess] CallGetTargetSlot param_size={}\n"), param_size);
        Output::send<LogLevel::Default>(
            STR("[PalAccess] === GetTargetSlot parameters ===\n"));
        for (auto* prop : func->ForEachProperty()) {
            if (!prop) continue;
            Output::send<LogLevel::Default>(
                STR("[PalAccess]   {} {}\n"),
                prop->GetClass().GetName(), prop->GetName());
        }
    });
    if (param_size == 0 || param_size > 256) return nullptr;
    std::vector<uint8_t> buffer(param_size, 0);
    slot_button->ProcessEvent(func, buffer.data());
    // The return is a single ObjectProperty (the UPalItemSlot pointer).
    for (auto* prop : func->ForEachProperty()) {
        if (!prop) continue;
        if (prop->GetClass().GetName() != L"ObjectProperty") continue;
        auto** ret = prop->ContainerPtrToValuePtr<Unreal::UObject*>(buffer.data());
        if (ret && *ret) {
            static std::once_flag s_ret_log;
            std::call_once(s_ret_log, [&]{
                auto* rcls = (*ret)->GetClassPrivate();
                Output::send<LogLevel::Default>(
                    STR("[PalAccess] GetTargetSlot returned class={}\n"),
                    rcls ? rcls->GetName() : L"<null>");
            });
            return *ret;
        }
    }
    static std::once_flag s_noret_log;
    std::call_once(s_noret_log, [&]{
        Output::send<LogLevel::Default>(
            STR("[PalAccess] GetTargetSlot returned null/empty\n"));
    });
    return nullptr;
}

// Read item id + stack count directly from UPalItemSlot's replicated
// UPROPERTYs. Confirmed layout from PalItemSlot.h (PalworldModdingKit)
// + runtime property dump:
//   IntProperty    StackCount
//   StructProperty ItemId        (FPalItemId)
//     NameProperty StaticId      <-- the live item id (e.g. "Wood")
//     StructProperty DynamicId
// Direct reads are reliable; UFunction calls (GetItemId, GetStackCount)
// were unreliable in this Palworld build — likely they're plain C++
// methods, not UFUNCTION-tagged, so they don't show up via the
// blueprint-call path. The properties themselves are always exposed.
std::wstring ReadFromTargetSlot(Unreal::UObject* target_slot, int32_t* out_count) {
    if (out_count) *out_count = 0;
    if (!target_slot) return {};
    auto* cls = target_slot->GetClassPrivate();
    if (!cls) return {};

    std::wstring name;
    for (auto* prop : cls->ForEachPropertyInChain()) {
        if (!prop) continue;
        auto ptype = prop->GetClass().GetName();
        auto pname = prop->GetName();

        // StackCount: replicated IntProperty.
        if (ptype == L"IntProperty" && pname == L"StackCount") {
            auto* val = prop->ContainerPtrToValuePtr<int32_t>(target_slot);
            if (val && out_count) *out_count = *val;
            continue;
        }

        // ItemId: replicated StructProperty FPalItemId. Walk it for
        // StaticId (NameProperty).
        if (ptype == L"StructProperty" && pname == L"ItemId") {
            auto* sp = reinterpret_cast<Unreal::FStructProperty*>(prop);
            auto* inner = sp->GetStruct().Get();
            if (!inner) continue;
            auto* struct_data = prop->ContainerPtrToValuePtr<uint8_t>(target_slot);
            if (!struct_data) continue;
            for (auto* leaf : inner->ForEachProperty()) {
                if (!leaf) continue;
                if (leaf->GetClass().GetName() != L"NameProperty") continue;
                if (leaf->GetName() != L"StaticId") continue;
                auto* fname = leaf->ContainerPtrToValuePtr<Unreal::FName>(struct_data);
                if (fname) {
                    auto raw = std::wstring(fname->ToString());
                    Output::send<LogLevel::Default>(
                        STR("[PalAccess] target-slot StaticId raw=\"{}\"\n"), raw);
                    if (!raw.empty() && raw != L"None") name = std::move(raw);
                }
                break;
            }
        }
    }
    return name;
}

// Diagnostic: one-shot dump of WBP_InventoryEquipment_C::SetCurrentSlot
// parameters. This UFunction fires every time Palworld binds a slot
// for display (every arrow press in the menu). If its parameter struct
// contains the slot's FName / display text, that's the authoritative
// path — read from there instead of the destination panel's RichText.
void HandleSetCurrentSlotDump(Unreal::UObject* Context, Unreal::FFrame& Stack) {
    if (!Context) return;
    auto cn = ClassNameOf(Context);
    auto fn = Stack.Node() ? Stack.Node()->GetName() : std::wstring();
    if (fn != L"SetCurrentSlot") return;
    static std::once_flag s_once;
    std::call_once(s_once, [&]{
        Output::send<LogLevel::Default>(
            STR("[PalAccess] === {}::SetCurrentSlot parameters ===\n"), cn);
        if (auto* func = Stack.Node()) {
            for (auto* prop : func->ForEachProperty()) {
                if (!prop) continue;
                Output::send<LogLevel::Default>(
                    STR("[PalAccess]   {} {}\n"),
                    prop->GetClass().GetName(), prop->GetName());
            }
        }
    });
    // Also walk one StructProperty parameter level deep (if any) — the
    // slot data shape we want is likely nested.
    static std::once_flag s_struct_once;
    std::call_once(s_struct_once, [&]{
        if (auto* func = Stack.Node()) {
            for (auto* prop : func->ForEachProperty()) {
                if (!prop) continue;
                if (prop->GetClass().GetName() != L"StructProperty") continue;
                auto* sp = reinterpret_cast<Unreal::FStructProperty*>(prop);
                auto* inner = sp->GetStruct().Get();
                if (!inner) continue;
                Output::send<LogLevel::Default>(
                    STR("[PalAccess]   [param struct {} fields]\n"),
                    inner->GetName());
                for (auto* sub : inner->ForEachProperty()) {
                    if (!sub) continue;
                    Output::send<LogLevel::Default>(
                        STR("[PalAccess]     {} {}\n"),
                        sub->GetClass().GetName(), sub->GetName());
                }
            }
        }
    });
}

void HandleInventoryInfoLifecycle(Unreal::UObject* Context, Unreal::FFrame& Stack) {
    if (!Context) return;
    auto cn = ClassNameOf(Context);
    if (cn != L"WBP_InventoryEquipment_ItemInfo_C") return;
    if (FunctionNameIs(Stack, L"Construct") ||
        FunctionNameIs(Stack, L"OnInitialized") ||
        FunctionNameIs(Stack, L"Change Display Mode")) {
        std::lock_guard<std::mutex> lock(g_inv_mtx);
        g_inv_item_info = Context;
    } else if (FunctionNameIs(Stack, L"Destruct")) {
        std::lock_guard<std::mutex> lock(g_inv_mtx);
        if (g_inv_item_info == Context) g_inv_item_info = nullptr;
    }
}

void HandleInventorySlotFocus(Unreal::UObject* Context, Unreal::FFrame& Stack) {
    if (!Context) return;
    auto cn = ClassNameOf(Context);
    if (cn != L"WBP_PalInGameMenuItemSlotButton_C") return;
    auto fn = Stack.Node() ? Stack.Node()->GetName() : std::wstring();
    if (fn != L"AnmEvent_Focus" &&
        fn != L"BP_OnHovered"    &&
        fn != L"AnmEvent_Hover") return;

    Output::send<LogLevel::Default>(
        STR("[PalAccess] inv-focus event={} ctx={:p}\n"),
        fn, static_cast<void*>(Context));

    // Slot-pointer dedup: a single keyboard/controller focus on a slot
    // fires up to three events (AnmEvent_Focus, BP_OnHovered,
    // AnmEvent_Hover) within ~10 ms. They all carry the same Context
    // pointer. The ItemInfo panel may update between them, which makes
    // the computed message string differ each time — message dedup
    // misses those. Pointer dedup catches all three cleanly. A user
    // arrowing away to a different slot is a different Context, so it
    // fires normally.
    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_inv_mtx);
        if (Context == g_inv_last_slot &&
            now - g_inv_last_slot_t < std::chrono::milliseconds(400)) return;
        g_inv_last_slot   = Context;
        g_inv_last_slot_t = now;
    }

    // Empty slot — short announce, skip name lookup entirely.
    const bool usable = SlotIsUsable(Context);
    if (!usable) {
        Speech::Get().FocusUpdate(L"Empty slot");
        return;
    }

    Unreal::UObject* info_panel = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_inv_mtx);
        info_panel = g_inv_item_info;
    }
    // Validate the cached pointer before dereferencing.
    if (info_panel) {
        auto* item = info_panel->GetObjectItem();
        if (!Unreal::UObjectArray::IsValid(item, /*evenIfPendingKill=*/false)) {
            std::lock_guard<std::mutex> lock(g_inv_mtx);
            g_inv_item_info = nullptr;
            info_panel = nullptr;
        }
    }
    // Cache-miss fallback: scan the live UObject array for an alive
    // WBP_InventoryEquipment_ItemInfo_C. Construct/Destruct ordering
    // can leave the cache stale across menu reopens.
    if (!info_panel) {
        Unreal::UObjectGlobals::ForEachUObject(
            [&](Unreal::UObject* obj, int32_t, int32_t) -> LoopAction {
                if (info_panel || !obj) return LoopAction::Continue;
                if (obj->GetName().starts_with(L"Default__")) return LoopAction::Continue;
                auto* cls = obj->GetClassPrivate();
                if (!cls) return LoopAction::Continue;
                if (cls->GetName() == L"WBP_InventoryEquipment_ItemInfo_C") {
                    info_panel = obj;
                    return LoopAction::Break;
                }
                return LoopAction::Continue;
            });
        if (info_panel) {
            std::lock_guard<std::mutex> lock(g_inv_mtx);
            g_inv_item_info = info_panel;
        }
    }
    // Primary path: GetTargetSlot() -> UPalItemSlot. Read ItemId
    // (StaticId FName) and StackCount directly as UPROPERTYs from the
    // returned slot. This is the same data Palworld replicates over
    // the network and what its own widgets render from.
    int32_t struct_count = 0;
    std::wstring raw_id;
    if (auto* target = CallGetTargetSlot(Context)) {
        raw_id = ReadFromTargetSlot(target, &struct_count);
    }
    // Fallbacks for builds where UPalItemSlot isn't directly accessible.
    if (raw_id.empty()) raw_id = ReadSlotItemViaGetter(Context, &struct_count);
    if (raw_id.empty()) raw_id = ReadSlotItemFromStruct(Context, &struct_count);

    // True-empty detection: per UPalItemSlot.h, StaticId "None" with
    // StackCount 0 means the slot has no bound item. `IsUsableSlot`
    // only means "this slot can hold an item" — empty grid cells in
    // the inventory still report IsUsableSlot=true. Treat None-id /
    // zero-count as actually empty.
    if (raw_id.empty() && struct_count <= 0) {
        Speech::Get().FocusUpdate(L"Empty slot");
        return;
    }

    std::wstring name;
    if (!raw_id.empty()) name = HumanizeRichTextId(raw_id);
    // Last-resort: info panel's RichText_ItemName (rarely needed now).
    if (name.empty()) name = ReadInventoryItemName(info_panel);
    // Count: prefer the live UPalItemSlot.StackCount value.
    std::wstring count;
    if (struct_count > 0) count = std::to_wstring(struct_count);
    else count = ReadInventorySlotCount(Context, info_panel);
    Output::send<LogLevel::Default>(
        STR("[PalAccess] inv-focus probe info_panel={:p} name=\"{}\" count=\"{}\"\n"),
        static_cast<void*>(info_panel), name, count);

    // One-shot dump of the slot button's "Item Info" struct fields the
    // first time a name read fails. Knowing the struct layout lets the
    // next iteration read the item id directly from the slot, bypassing
    // the (sometimes-stale) ItemInfo panel entirely.
    if (name.empty()) {
        static std::once_flag s_iteminfo_dump;
        std::call_once(s_iteminfo_dump, [&]{
            auto* cls = Context->GetClassPrivate();
            if (!cls) return;
            for (auto* prop : cls->ForEachPropertyInChain()) {
                if (!prop) continue;
                if (prop->GetClass().GetName() != L"StructProperty") continue;
                if (prop->GetName() != L"Item Info") continue;
                auto* sp = reinterpret_cast<Unreal::FStructProperty*>(prop);
                auto* inner = sp->GetStruct().Get();
                if (!inner) break;
                Output::send<LogLevel::Default>(
                    STR("[PalAccess] === slot Item Info struct ({}) fields ===\n"),
                    inner->GetName());
                int n = 0;
                for (auto* sub : inner->ForEachProperty()) {
                    if (!sub) continue;
                    if (++n > 60) break;
                    Output::send<LogLevel::Default>(
                        STR("[PalAccess]   {} {}\n"),
                        sub->GetClass().GetName(), sub->GetName());
                }
                break;
            }
            // Also dump GetItemAndNum's parameter list, since calling
            // that UFunction is another route to read item id + count.
            if (auto* func = Context->GetFunctionByName(L"GetItemAndNum")) {
                Output::send<LogLevel::Default>(
                    STR("[PalAccess] === GetItemAndNum parameters ===\n"));
                for (auto* prop : func->ForEachProperty()) {
                    if (!prop) continue;
                    Output::send<LogLevel::Default>(
                        STR("[PalAccess]   {} {}\n"),
                        prop->GetClass().GetName(), prop->GetName());
                }
            }
        });
    }
    if (IsPlaceholderText(name) || IsNotificationPlaceholder(name)) name.clear();

    // Strip leading zeros from the count; "000000" is Palworld's
    // unbound-widget placeholder.
    {
        size_t first_nz = count.find_first_not_of(L'0');
        if (first_nz == std::wstring::npos) count.clear();
        else if (first_nz > 0) count = count.substr(first_nz);
    }
    // Treat the placeholder-name string as empty.
    if (IsPlaceholderText(name) || IsNotificationPlaceholder(name)) name.clear();

    std::wstring msg;
    if (!name.empty()) {
        std::wstring item_count = count.empty() ? std::wstring(L"1") : count;
        msg  = name;
        msg += L", ";
        msg += item_count;
    } else {
        // No name yet — speak whatever we have so the player at least
        // hears that they moved onto a non-empty slot. The next hover
        // (once Palworld binds the info panel) will speak the real
        // name and dedup-skip if identical.
        std::wstring item_count = count.empty() ? std::wstring(L"1") : count;
        msg = L"Item, " + item_count;
    }

    // Name+count dedup with a generous 1200 ms window. When a player
    // has several slots of the same single-quantity item (multiple
    // Wooden Clubs, etc.), Palworld hides the count widget so the
    // message is just the bare name — and the user perceives every
    // arrow press as a duplicate. Items with distinct counts produce
    // distinct messages ("Red Berries, 5" vs "Red Berries, 12") which
    // bypass the dedup naturally; only truly-identical announces are
    // suppressed. A user lingering on a slot >1.2 s and arrowing back
    // hears it re-announce.
    // Only apply name-string dedup when the name actually resolved.
    // Otherwise every slot would produce "Item, 1" and dedup-skip
    // after the first, leaving the user with silence.
    if (!name.empty()) {
        std::lock_guard<std::mutex> lock(g_inv_mtx);
        if (msg == g_inv_last_msg &&
            now - g_inv_last_msg_t < std::chrono::milliseconds(1200)) {
            Output::send<LogLevel::Default>(
                STR("[PalAccess] inv-focus dedup-skip \"{}\"\n"), msg);
            return;
        }
        g_inv_last_msg   = msg;
        g_inv_last_msg_t = now;
    }
    Output::send<LogLevel::Default>(
        STR("[PalAccess] inv-focus ANNOUNCE \"{}\"\n"), msg);
    Speech::Get().FocusUpdate(std::wstring_view(msg));
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
    // Quest start/clear popup is read by HandleQuestNotification via its
    // inner text blocks — the parent widget exposes placeholder text
    // (クエスト名) at Construct time which would announce as garbage.
    if (cn == L"WBP_InGame_Quest_StartClear_C") return;

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

// Quest start/clear notification. Two entry points:
//   1. WBP_InGame_Quest_StartClear_C::Construct — the popup. Cache the
//      widget so Hooks::Tick can poll its inner text blocks once the
//      ubergraph resolves the placeholder.
//   2. WBP_Ingame_QuestBoard_C::UpdateTrackingQuestDetail — fires right
//      after mission start with the resolved title bound. Read the
//      questboard's text content here as a more reliable source than
//      the popup (which can keep the クエスト名 placeholder if the
//      quest title is missing from Palworld's English locale data).
void HandleQuestNotification(Unreal::UObject* Context, Unreal::FFrame& Stack) {
    if (!Context) return;
    auto* cls = Context->GetClassPrivate();
    if (!cls) return;
    auto cn = cls->GetName();

    if (FunctionNameIs(Stack, L"Construct") &&
        cn == L"WBP_InGame_Quest_StartClear_C") {
        std::lock_guard<std::mutex> lock(g_quest_mtx);
        g_quest_popup        = Context;
        g_quest_construct_at = std::chrono::steady_clock::now();
        g_quest_announced    = false;
        return;
    }

    // Questboard tracker updated — the resolved title is rendered in
    // its widget tree. Pull all visible text, log it for discovery,
    // and let the popup poll consume the title from a sibling source
    // if needed. We don't announce from here yet (avoid duplicates with
    // the popup announce) but we DO log so we can confirm the field.
    if (cn == L"WBP_Ingame_QuestBoard_C" &&
        (FunctionNameIs(Stack, L"UpdateTrackingQuestDetail") ||
         FunctionNameIs(Stack, L"UpdateQuestDetail"))) {
        auto labels = ExtractWidgetLabels(Context, 0);
        Output::send<LogLevel::Default>(
            STR("[PalAccess] questboard update via {} labels=\"{}\"\n"),
            Stack.Node() ? Stack.Node()->GetName() : L"<?>", labels);
        // Hand it to the poll as a candidate title source.
        std::lock_guard<std::mutex> lock(g_quest_mtx);
        if (!g_quest_announced) {
            g_questboard_labels = std::move(labels);
            g_questboard_at     = std::chrono::steady_clock::now();
        }
    }
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

// Read the live FText value of a named text-block property on a widget.
// Returns the resolved string or empty if the property is missing or the
// FText is empty.
static std::wstring ReadTextBlockText(Unreal::UObject* widget,
                                      std::wstring_view block_property_name) {
    if (!widget) return {};
    auto* cls = widget->GetClassPrivate();
    if (!cls) return {};
    for (auto* prop : cls->ForEachPropertyInChain()) {
        if (!prop) continue;
        if (prop->GetClass().GetName() != L"ObjectProperty") continue;
        if (prop->GetName() != block_property_name) continue;
        auto** child_ptr = prop->ContainerPtrToValuePtr<Unreal::UObject*>(widget);
        if (!child_ptr || !*child_ptr) return {};
        Unreal::UObject* tb = *child_ptr;
        auto* tb_cls = tb->GetClassPrivate();
        if (!tb_cls) return {};
        // Walk the text block's own properties looking for an FText
        // named "Text" (the standard UTextBlock field).
        for (auto* tp : tb_cls->ForEachPropertyInChain()) {
            if (!tp) continue;
            if (tp->GetClass().GetName() != L"TextProperty") continue;
            if (tp->GetName() != L"Text") continue;
            auto* ftext = tp->ContainerPtrToValuePtr<Unreal::FText>(tb);
            if (!ftext) return {};
            return std::wstring(ftext->ToString());
        }
        return {};
    }
    return {};
}

// Per-frame poll. Drives two deferred-discovery flows:
//   1. Patch-notes WebBrowser GetUrl polling (see g_news_*).
//   2. Quest start/clear popup title polling (see g_quest_*) — reads
//      Text_Quest_New / Text_Quest_Complete each tick until the
//      ubergraph resolves the placeholder, then speaks the title.
void Hooks::Tick() {
    // ---- Quest start/clear poll ----
    // Text_Quest_New / Text_Quest_Complete hold the static labels
    // ("Mission Started" / "Mission Cleared"). The actual quest title
    // lives in a different text block (likely something like
    // Text_QuestTitle / Text_Description) reachable a level deeper in
    // the widget tree. ExtractWidgetLabels walks 2 levels and joins
    // every visible non-placeholder TextProperty with ", " — we wait
    // for the joined string to grow beyond just the static labels and
    // then split out the title.
    {
        std::lock_guard<std::mutex> lock(g_quest_mtx);
        if (g_quest_popup && !g_quest_announced) {
            auto* item = g_quest_popup->GetObjectItem();
            if (!Unreal::UObjectArray::IsValid(item, /*evenIfPendingKill=*/false)) {
                g_quest_popup = nullptr;
            } else {
                const auto now = std::chrono::steady_clock::now();
                const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - g_quest_construct_at).count();
                if (since > 3000) {
                    g_quest_announced = true;
                    g_quest_popup     = nullptr;
                } else {
                    auto split = [](const std::wstring& s) {
                        std::vector<std::wstring> out;
                        size_t start = 0;
                        while (start < s.size()) {
                            auto end = s.find(L", ", start);
                            if (end == std::wstring::npos) {
                                out.push_back(s.substr(start));
                                break;
                            }
                            out.push_back(s.substr(start, end - start));
                            start = end + 2;
                        }
                        return out;
                    };
                    auto popup_labels = ExtractWidgetLabels(g_quest_popup, 0);
                    auto popup_frags  = split(popup_labels);

                    bool saw_started = false, saw_cleared = false;
                    std::wstring title;
                    auto consider = [&](const std::vector<std::wstring>& frags) {
                        for (auto& f : frags) {
                            if (f.empty()) continue;
                            if (IsNotificationPlaceholder(f)) continue;
                            if (f == L"Mission Started" || f == L"MISSION STARTED" ||
                                f == L"Quest Started"   || f == L"QUEST STARTED") {
                                saw_started = true;
                                continue;
                            }
                            if (f == L"Mission Cleared" || f == L"MISSION CLEARED" ||
                                f == L"Mission Complete" || f == L"Quest Complete" ||
                                f == L"Quest Cleared") {
                                saw_cleared = true;
                                continue;
                            }
                            if (title.empty()) title = f;
                        }
                    };
                    consider(popup_frags);

                    // Fallback: if the popup's text block still holds
                    // the placeholder (which happens when Palworld's
                    // English locale data is missing the title), pull
                    // the title from the questboard tracker update that
                    // fires around the same time.
                    if (title.empty() && !g_questboard_labels.empty()) {
                        consider(split(g_questboard_labels));
                    }

                    if ((saw_started || saw_cleared) && !title.empty()) {
                        std::wstring msg = saw_started
                            ? L"Mission started, " : L"Mission cleared, ";
                        msg += title;
                        Speech::Get().Announce(std::wstring_view(msg));
                        Output::send<LogLevel::Default>(
                            STR("[PalAccess] quest popup resolved: \"{}\" (popup=\"{}\" board=\"{}\")\n"),
                            title, popup_labels, g_questboard_labels);
                        g_quest_announced     = true;
                        g_quest_popup         = nullptr;
                        g_questboard_labels.clear();
                    } else {
                        // Per-poll snapshot log so we can see what's
                        // actually in the widget at each tick. Throttled
                        // by since-construct rounding.
                        static long long s_last_log_ms = -1;
                        long long bucket = since / 100;  // every 100 ms
                        if (bucket != s_last_log_ms) {
                            s_last_log_ms = bucket;
                            Output::send<LogLevel::Default>(
                                STR("[PalAccess] quest poll t={}ms popup=\"{}\" board=\"{}\"\n"),
                                since, popup_labels, g_questboard_labels);
                        }
                    }
                }
            }
        }
    }

    std::lock_guard<std::mutex> lock(g_news_mtx);
    if (g_news_url_logged) return;
    if (!g_news_inner) return;
    auto* item = g_news_inner->GetObjectItem();
    if (!Unreal::UObjectArray::IsValid(item, /*evenIfPendingKill=*/false)) {
        g_news_inner = nullptr;
        return;
    }
    // Bail after 5 seconds — if the URL still isn't set by then it
    // probably never will be, and we don't want to spin forever.
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            now - g_news_construct_at).count() > 5000) {
        Output::send<LogLevel::Default>(
            STR("[PalAccess] gave up polling WebBrowser GetUrl after 5s\n"));
        g_news_url_logged = true;
        g_news_inner      = nullptr;
        return;
    }
    auto* func = g_news_inner->GetFunctionByName(L"GetUrl");
    if (!func) {
        Output::send<LogLevel::Default>(
            STR("[PalAccess] WebBrowser has no GetUrl UFunction\n"));
        g_news_url_logged = true;
        g_news_inner      = nullptr;
        return;
    }
    struct Parms { Unreal::FString Ret; };
    Parms p{};
    g_news_inner->ProcessEvent(func, &p);
    if (p.Ret.IsEmpty()) return;  // not loaded yet — keep polling
    const wchar_t* url_c = p.Ret.GetCharArray().GetData();
    if (!url_c) return;
    Output::send<LogLevel::Default>(
        STR("[PalAccess] WebBrowser_News URL: \"{}\"\n"), url_c);
    // Kick the fetcher with the captured URL. It runs on a background
    // thread, strips HTML, filters to English paragraphs, and queues them
    // through Speech::Queue.
    PatchNotes::Fetch(std::wstring_view(url_c));
    g_news_url_logged = true;
    g_news_inner      = nullptr;
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

    // The "Welcome to Palworld. Main menu loading." announce that used
    // to fire here was removed — it competed with the patch-notes
    // screen's announce in the same cluster window and pushed it back.
    // Patch notes is the first useful event on launch.
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
    HandleQuestNotification(Context, Stack);
    HandleSystemNotification(Context, Stack);
    HandleConstructionInfoUpdate(Context, Stack);
    HandleConstructionItemFocus(Context, Stack);
    HandleConstructionMaterialRow(Context, Stack);
    HandleConstructionTabSelect(Context, Stack);
    HandleInventoryInfoLifecycle(Context, Stack);
    HandleInventorySlotFocus(Context, Stack);
    HandleRecipeSlotFocus(Context, Stack);
    HandleRecipeSlotClick(Context, Stack);
    HandleSetCurrentSlotDump(Context, Stack);
    HandleInGameMenuTabSelect(Context, Stack);
}

// Legacy placeholder dispatchers kept as no-ops — declared in the header so the
// translation unit still satisfies the interface. Replaced by the named handlers above.
void Hooks::TryAnnounceDialog(Unreal::UObject*, Unreal::FFrame&)      {}
void Hooks::TryAnnounceMenuFocus(Unreal::UObject*, Unreal::FFrame&)   {}
void Hooks::TryAnnounceItemPickup(Unreal::UObject*, Unreal::FFrame&)  {}
void Hooks::TryAnnouncePalEvent(Unreal::UObject*, Unreal::FFrame&)    {}

} // namespace PalAccess
