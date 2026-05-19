#include "Hotkeys.hpp"
#include "Hooks.hpp"
#include "Nav.hpp"
#include "Speech.hpp"

// UE4SS headers must come BEFORE windows.h: the latter pulls in legacy
// `min`/`max` macros that collide with `std::numeric_limits<T>::max()`
// inside UE's UnrealString / Hook constants. NOMINMAX suppresses them
// regardless, but ordering keeps things robust if any header changes.
#include <DynamicOutput/Output.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Constructs/Loop.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UScriptStruct.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <Xinput.h>
#pragma comment(lib, "Xinput.lib")

#include <functional>

#include <array>
#include <cmath>
#include <cwchar>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

using namespace RC;

namespace PalAccess {

namespace {

std::mutex          g_mtx;
Unreal::UObject*    g_player_pawn  = nullptr;  // BP_Player_<Sex>_C
Unreal::UObject*    g_player_state = nullptr;  // BP_PalPlayerState_C

bool LooksLikePlayer(std::wstring_view class_name) {
    // Real classes observed: BP_Player_Female_C, BP_Player_Male_C.
    // Exclude BP_Player_ForUI_C (used for UI dressing, not gameplay).
    if (class_name.starts_with(L"BP_Player_") &&
        class_name != L"BP_Player_ForUI_C") {
        return true;
    }
    // Defensive: catch hypothetical Pal-prefixed variants too.
    if (class_name.starts_with(L"BP_PalPlayerCharacter")) return true;
    return false;
}

bool LooksLikePlayerState(std::wstring_view class_name) {
    return class_name.starts_with(L"BP_PalPlayerState");
}

bool IsDefaultObject(Unreal::UObject* obj) {
    if (!obj) return true;
    return obj->GetName().starts_with(L"Default__");
}

// True if `obj` still occupies a live slot in the UObject array. Returns
// false for null, freed, or garbage-collected pointers so we don't
// dereference a destroyed widget.
bool IsLiveUObject(Unreal::UObject* obj) {
    if (!obj) return false;
    auto* item = obj->GetObjectItem();
    return Unreal::UObjectArray::IsValid(item, /*evenIfPendingKill=*/false);
}

Unreal::UObject* CachedPlayer() {
    std::lock_guard<std::mutex> lock(g_mtx);
    return g_player_pawn;
}
Unreal::UObject* CachedPlayerState() {
    std::lock_guard<std::mutex> lock(g_mtx);
    return g_player_state;
}

void SetCachedPlayer(Unreal::UObject* obj) {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_player_pawn = obj;
}
void SetCachedPlayerState(Unreal::UObject* obj) {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_player_state = obj;
}

// Scan the live UObject array for a player-class instance. Last-resort
// fallback when the BeginPlay cache hasn't seen one yet (e.g. mod loaded
// mid-game).
struct PlayerScanResult {
    Unreal::UObject* pawn  = nullptr;
    Unreal::UObject* state = nullptr;
};

PlayerScanResult ScanForPlayer() {
    PlayerScanResult out;
    Unreal::UObjectGlobals::ForEachUObject(
        [&](Unreal::UObject* obj, int32_t /*idx*/, int32_t /*chunk*/) -> LoopAction {
            if (out.pawn && out.state) return LoopAction::Break;
            if (!obj || IsDefaultObject(obj)) return LoopAction::Continue;
            auto cls = obj->GetClassPrivate();
            if (!cls) return LoopAction::Continue;
            auto cn = cls->GetName();
            if (!out.pawn  && LooksLikePlayer(cn))      out.pawn  = obj;
            if (!out.state && LooksLikePlayerState(cn)) out.state = obj;
            return LoopAction::Continue;
        });
    return out;
}

void EnsureCached() {
    // Drop any pointer that's no longer in the UObject array (respawn /
    // zone transition / world reload would otherwise leave us with a
    // dangling pointer, and dereferencing it silently fails the hotkey).
    if (auto* p = CachedPlayer();      p && !IsLiveUObject(p)) SetCachedPlayer(nullptr);
    if (auto* s = CachedPlayerState(); s && !IsLiveUObject(s)) SetCachedPlayerState(nullptr);
    if (CachedPlayer() && CachedPlayerState()) return;
    auto scan = ScanForPlayer();
    if (scan.pawn  && !CachedPlayer())      SetCachedPlayer(scan.pawn);
    if (scan.state && !CachedPlayerState()) SetCachedPlayerState(scan.state);
}

Unreal::UObject* GetPlayer() {
    EnsureCached();
    return CachedPlayer();
}
Unreal::UObject* GetPlayerState() {
    EnsureCached();
    return CachedPlayerState();
}

// Read a numeric UProperty (int/float/double) into a double from a raw
// base pointer. The base may be a UObject start (for direct properties) or
// a struct's start (for FStructProperty inner fields).
std::optional<double> ReadNumericAt(Unreal::FProperty* prop, uint8_t* base) {
    if (!prop || !base) return std::nullopt;
    auto ptype = prop->GetClass().GetName();
    auto* addr = base + prop->GetOffset_Internal();
    if (ptype == L"IntProperty")     return static_cast<double>(*reinterpret_cast<int32_t*>(addr));
    if (ptype == L"Int64Property")   return static_cast<double>(*reinterpret_cast<int64_t*>(addr));
    if (ptype == L"UInt32Property")  return static_cast<double>(*reinterpret_cast<uint32_t*>(addr));
    if (ptype == L"UInt64Property")  return static_cast<double>(*reinterpret_cast<uint64_t*>(addr));
    if (ptype == L"Int16Property")   return static_cast<double>(*reinterpret_cast<int16_t*>(addr));
    if (ptype == L"UInt16Property")  return static_cast<double>(*reinterpret_cast<uint16_t*>(addr));
    if (ptype == L"Int8Property")    return static_cast<double>(*reinterpret_cast<int8_t*>(addr));
    if (ptype == L"ByteProperty")    return static_cast<double>(*reinterpret_cast<uint8_t*>(addr));
    if (ptype == L"FloatProperty")   return static_cast<double>(*reinterpret_cast<float*>(addr));
    if (ptype == L"DoubleProperty")  return *reinterpret_cast<double*>(addr);
    return std::nullopt;
}

// Palworld scales fixed-point stats (FFixedPoint64-like wrappers) by 1000:
// the displayed value 500 HP is stored as int64 500000. When the value is
// the inner numeric of a stat struct (Hp / MaxHP / SP / etc.) and it's
// int64-typed, divide by this scale.
constexpr double kFixedPointScale = 1000.0;

// Read a numeric value from a property, transparently unwrapping
// struct-wrapped values: if the property is a struct, read the FIRST
// numeric inner field. Int64 inner fields are treated as FFixedPoint64
// and rescaled.
std::optional<double> ReadNumericOrStructValueAt(Unreal::FProperty* prop, uint8_t* base) {
    if (!prop || !base) return std::nullopt;
    if (auto v = ReadNumericAt(prop, base)) return v;
    if (prop->GetClass().GetName() != L"StructProperty") return std::nullopt;
    auto* sp = reinterpret_cast<Unreal::FStructProperty*>(prop);
    Unreal::UScriptStruct* inner = sp->GetStruct().Get();
    if (!inner) return std::nullopt;
    uint8_t* inner_base = base + prop->GetOffset_Internal();
    for (auto* ip : inner->ForEachProperty()) {
        if (!ip) continue;
        if (auto v = ReadNumericAt(ip, inner_base)) {
            // Inner int64 inside a struct wrapper is treated as fixed-point.
            if (ip->GetClass().GetName() == L"Int64Property") return *v / kFixedPointScale;
            return v;
        }
    }
    return std::nullopt;
}

std::optional<double> ReadNumeric(Unreal::FProperty* prop, Unreal::UObject* container) {
    return ReadNumericAt(prop, reinterpret_cast<uint8_t*>(container));
}

// Find a numeric property matching the given name predicate and return its
// value. Walks the inheritance chain so inherited stat fields are caught.
std::optional<double> FindNumeric(Unreal::UObject* obj,
                                  std::function<bool(std::wstring_view)> match) {
    if (!obj) return std::nullopt;
    auto cls = obj->GetClassPrivate();
    if (!cls) return std::nullopt;
    for (auto* prop : cls->ForEachPropertyInChain()) {
        if (!prop) continue;
        if (!match(prop->GetName())) continue;
        if (auto v = ReadNumeric(prop, obj)) return v;
    }
    return std::nullopt;
}

std::wstring FormatNumber(double v) {
    wchar_t buf[64];
    if (std::abs(v - std::round(v)) < 1e-3) {
        std::swprintf(buf, 64, L"%lld", static_cast<long long>(std::llround(v)));
    } else {
        std::swprintf(buf, 64, L"%.1f", v);
    }
    return buf;
}

// Read "Current" and "Max" by name patterns. Both lowercased for the test.
struct StatPair { std::optional<double> cur, max; };

// Property-name fragments that look stat-shaped but are actually
// multipliers / debug fields / UI offsets / regen rates. Anything containing
// these gets skipped — too many false positives like MaxHPRate_ForTowerBoss.
bool IsNoiseStatField(std::wstring_view lower) {
    auto has = [&](std::wstring_view needle) {
        return lower.find(needle) != std::wstring::npos;
    };
    return has(L"dying")       || has(L"rate")        || has(L"buff")
        || has(L"debuff")      || has(L"multiplier")  || has(L"modifier")
        || has(L"gauge")       || has(L"override")    || has(L"debug")
        || has(L"delta")       || has(L"natural")     || has(L"recovery")
        || has(L"regen")       || has(L"overheat")    || has(L"offset")
        || has(L"resistance")  || has(L"reduction")   || has(L"scale")
        || has(L"threshold")   || has(L"forui")       || has(L"forhud");
}

std::wstring ToLower(std::wstring_view in) {
    std::wstring out;
    out.reserve(in.size());
    for (auto c : in) out.push_back(static_cast<wchar_t>(std::towlower(c)));
    return out;
}

// Walk every property of `container_type` (a UStruct OR a UClass — both have
// the same ForEachProperty surface) looking at base+offset, including
// recursing into nested FStructProperty fields up to `struct_depth`.
void AccumulateNumericMatches(Unreal::UStruct* container_type, uint8_t* container_base,
                              std::wstring_view keyword, StatPair& out,
                              int struct_depth) {
    if (!container_type || !container_base) return;
    for (auto* prop : container_type->ForEachProperty()) {
        if (!prop) continue;
        auto lower = ToLower(prop->GetName());
        auto ptype = prop->GetClass().GetName();

        // Numeric match at this level.
        const bool kw_match = lower.find(keyword) != std::wstring::npos;
        if (kw_match && !IsNoiseStatField(lower)) {
            if (auto val = ReadNumericAt(prop, container_base)) {
                const bool is_max = lower.find(L"max") != std::wstring::npos;
                if (is_max) { if (!out.max) out.max = val; }
                else        { if (!out.cur) out.cur = val; }
                continue;
            }
        }

        // Recurse into nested struct fields (e.g. SaveParameter.HP.Value).
        if (ptype == L"StructProperty" && struct_depth < 3) {
            auto* sp = reinterpret_cast<Unreal::FStructProperty*>(prop);
            Unreal::UScriptStruct* inner = sp->GetStruct().Get();
            if (!inner) continue;
            uint8_t* inner_base = container_base + prop->GetOffset_Internal();
            AccumulateNumericMatches(inner, inner_base, keyword, out, struct_depth + 1);
            if (out.cur && out.max) return;
        }
    }
}

void AccumulateStatPair(Unreal::UObject* obj, std::wstring_view keyword, StatPair& out) {
    if (!obj) return;
    auto cls = obj->GetClassPrivate();
    if (!cls) return;
    // ForEachPropertyInChain gives us inherited fields too. We iterate via
    // the class itself for inheritance, plus struct recursion for nested data.
    for (auto* prop : cls->ForEachPropertyInChain()) {
        if (!prop) continue;
        auto lower = ToLower(prop->GetName());
        auto ptype = prop->GetClass().GetName();

        if (lower.find(keyword) != std::wstring::npos && !IsNoiseStatField(lower)) {
            if (auto val = ReadNumeric(prop, obj)) {
                const bool is_max = lower.find(L"max") != std::wstring::npos;
                if (is_max) { if (!out.max) out.max = val; }
                else        { if (!out.cur) out.cur = val; }
                continue;
            }
        }

        if (ptype == L"StructProperty") {
            auto* sp = reinterpret_cast<Unreal::FStructProperty*>(prop);
            Unreal::UScriptStruct* inner = sp->GetStruct().Get();
            if (!inner) continue;
            uint8_t* inner_base = reinterpret_cast<uint8_t*>(obj) + prop->GetOffset_Internal();
            AccumulateNumericMatches(inner, inner_base, keyword, out, 0);
            if (out.cur && out.max) return;
        }
    }
}

// True if the property name suggests a stat-bearing sub-object: a component,
// a parameter holder, status, network handle, or a Pal-prefixed record/data
// blob. Filters out movement / mesh / camera / collision noise.
bool LooksLikeStatComponent(std::wstring_view name) {
    auto contains = [&](std::wstring_view needle) {
        return name.find(needle) != std::wstring::npos;
    };
    return contains(L"Parameter") || contains(L"Status") ||
           contains(L"Stat")      || contains(L"Record") ||
           contains(L"Data")      || contains(L"State")  ||
           contains(L"Handle")    ||
           name == L"ActionComponent" || name == L"DamageReactionComponent";
}

// Read stat pair by walking the object's properties AND recursing into
// stat-bearing component sub-objects up to `max_depth` levels.
// Palworld places actual HP/SP values two levels deep:
//   pawn -> CharacterParameterComponent -> IndividualParameter -> HP/SP
void ReadStatPairRecursive(Unreal::UObject* obj, std::wstring_view keyword,
                           StatPair& out, int depth, int max_depth,
                           std::unordered_set<uintptr_t>& visited) {
    if (!obj || depth > max_depth) return;
    auto key = reinterpret_cast<uintptr_t>(obj);
    if (!visited.insert(key).second) return;

    AccumulateStatPair(obj, keyword, out);
    if (out.cur && out.max) return;
    if (depth == max_depth) return;

    auto cls = obj->GetClassPrivate();
    if (!cls) return;
    for (auto* prop : cls->ForEachPropertyInChain()) {
        if (!prop) continue;
        if (prop->GetClass().GetName() != L"ObjectProperty") continue;
        if (!LooksLikeStatComponent(prop->GetName())) continue;
        auto** child_ptr = prop->ContainerPtrToValuePtr<Unreal::UObject*>(obj);
        if (!child_ptr || !*child_ptr) continue;
        ReadStatPairRecursive(*child_ptr, keyword, out, depth + 1, max_depth, visited);
        if (out.cur && out.max) return;
    }
}

StatPair ReadStatPair(Unreal::UObject* obj, std::wstring_view keyword) {
    StatPair out;
    std::unordered_set<uintptr_t> visited;
    // depth 3 gets us pawn -> CharacterParameterComponent -> IndividualParameter
    // -> ParentParameter / EquipItemContainer etc.
    ReadStatPairRecursive(obj, keyword, out, 0, /*max_depth=*/3, visited);
    return out;
}

// Find an EXACT-named field (case-insensitive) anywhere reachable from `root`
// — walks UObject properties, struct contents, and stat-shaped sub-objects.
// Returns the numeric value, unwrapping struct-wrapped stats automatically.
std::optional<double> FindFieldValueRecursive(
        Unreal::UObject* obj, std::wstring_view target_lower,
        int depth, int max_depth,
        std::unordered_set<uintptr_t>& visited);

std::optional<double> ScanContainerForField(
        Unreal::UStruct* container_type, uint8_t* container_base,
        std::wstring_view target_lower, int struct_depth) {
    if (!container_type || !container_base) return std::nullopt;
    for (auto* prop : container_type->ForEachProperty()) {
        if (!prop) continue;
        auto lower = ToLower(prop->GetName());
        if (lower == target_lower) {
            if (auto v = ReadNumericOrStructValueAt(prop, container_base)) return v;
        }
        if (prop->GetClass().GetName() == L"StructProperty" && struct_depth < 4) {
            auto* sp = reinterpret_cast<Unreal::FStructProperty*>(prop);
            Unreal::UScriptStruct* inner = sp->GetStruct().Get();
            if (!inner) continue;
            uint8_t* inner_base = container_base + prop->GetOffset_Internal();
            if (auto v = ScanContainerForField(inner, inner_base, target_lower, struct_depth + 1))
                return v;
        }
    }
    return std::nullopt;
}

std::optional<double> FindFieldValueRecursive(
        Unreal::UObject* obj, std::wstring_view target_lower,
        int depth, int max_depth,
        std::unordered_set<uintptr_t>& visited) {
    if (!obj || depth > max_depth) return std::nullopt;
    auto key = reinterpret_cast<uintptr_t>(obj);
    if (!visited.insert(key).second) return std::nullopt;

    auto cls = obj->GetClassPrivate();
    if (!cls) return std::nullopt;

    // 1) Walk this object's properties + nested structs.
    if (auto v = ScanContainerForField(cls, reinterpret_cast<uint8_t*>(obj), target_lower, 0))
        return v;
    if (depth == max_depth) return std::nullopt;

    // 2) Recurse into stat-shaped component children.
    for (auto* prop : cls->ForEachPropertyInChain()) {
        if (!prop) continue;
        if (prop->GetClass().GetName() != L"ObjectProperty") continue;
        if (!LooksLikeStatComponent(prop->GetName())) continue;
        auto** child_ptr = prop->ContainerPtrToValuePtr<Unreal::UObject*>(obj);
        if (!child_ptr || !*child_ptr) continue;
        if (auto v = FindFieldValueRecursive(*child_ptr, target_lower, depth + 1, max_depth, visited))
            return v;
    }
    return std::nullopt;
}

std::optional<double> FindFieldValue(Unreal::UObject* root, std::wstring_view field_name) {
    std::wstring target = ToLower(field_name);
    std::unordered_set<uintptr_t> visited;
    return FindFieldValueRecursive(root, target, 0, /*max_depth=*/4, visited);
}

// Search a list of candidate field names; return the first that hits.
std::optional<double> FindAnyField(Unreal::UObject* root,
                                   std::initializer_list<std::wstring_view> names) {
    for (auto name : names) {
        if (auto v = FindFieldValue(root, name)) return v;
    }
    return std::nullopt;
}

// Speak a stat. Always reads live from SaveParameter (cur and max).
// Palworld doesn't store player MaxHP — it's computed at runtime — so HP
// will announce current only, no "out of X" suffix.
void SpeakStatBy(std::wstring_view label,
                 std::initializer_list<std::wstring_view> cur_fields,
                 std::initializer_list<std::wstring_view> max_fields,
                 std::wstring_view not_found_msg) {
    auto* p = GetPlayer();
    auto* s = GetPlayerState();
    if (!p && !s) {
        Speech::Get().Announce(L"Player not loaded.");
        return;
    }
    std::optional<double> cur, max;
    for (auto* root : {p, s}) {
        if (!root) continue;
        if (!cur) for (auto n : cur_fields) if ((cur = FindFieldValue(root, n))) break;
        if (!max) for (auto n : max_fields) if ((max = FindFieldValue(root, n))) break;
        if (cur && (max_fields.size() == 0 || (max && *max > 0))) break;
    }
    if (!cur) {
        Speech::Get().Announce(not_found_msg);
        return;
    }
    std::wstring msg(label);
    msg += L" ";
    msg += FormatNumber(*cur);
    if (max && *max > 0.0) {
        msg += L" out of ";
        msg += FormatNumber(*max);
    }
    Speech::Get().Announce(std::wstring_view(msg));
}

// Dump inner properties of a script struct, recursing one level into
// nested structs whose names look stat-shaped. Bounded so a giant save
// struct doesn't flood the log.
void DumpScriptStructInner(Unreal::UScriptStruct* script_struct,
                           const std::wstring& parent_label, int depth = 0) {
    if (!script_struct || depth > 1) return;
    Output::send<LogLevel::Default>(
        STR("[PalAccess] === {} struct fields ({}) ===\n"),
        parent_label, script_struct->GetName());
    int n = 0;
    for (auto* prop : script_struct->ForEachProperty()) {
        if (!prop) continue;
        if (++n > 60) { Output::send<LogLevel::Default>(STR("[PalAccess]   ...\n")); break; }
        Output::send<LogLevel::Default>(
            STR("[PalAccess]   {} {}\n"),
            prop->GetClass().GetName(), prop->GetName());
        if (prop->GetClass().GetName() == L"StructProperty") {
            auto inner_lower = ToLower(prop->GetName());
            const bool stat_shaped =
                inner_lower == L"hp"        || inner_lower == L"maxhp"     ||
                inner_lower == L"sp"        || inner_lower == L"maxsp"     ||
                inner_lower == L"mp"        || inner_lower == L"maxmp"     ||
                inner_lower == L"shieldhp"  || inner_lower == L"shieldmaxhp" ||
                inner_lower == L"sanity"    || inner_lower == L"sanityvalue";
            if (stat_shaped) {
                auto* sp = reinterpret_cast<Unreal::FStructProperty*>(prop);
                std::wstring sub_label = parent_label + L"." + prop->GetName();
                DumpScriptStructInner(sp->GetStruct().Get(), sub_label, depth + 1);
            }
        }
    }
}

void DumpClass(Unreal::UObject* obj, const wchar_t* label) {
    if (!obj) return;
    auto cls = obj->GetClassPrivate();
    if (!cls) return;
    auto cn = cls->GetName();
    Output::send<LogLevel::Default>(
        STR("[PalAccess] === {} diagnose ({}, instance {}) ===\n"),
        label, cn, obj->GetName());
    int n = 0;
    for (auto* prop : cls->ForEachPropertyInChain()) {
        if (!prop) continue;
        if (++n > 200) break;
        Output::send<LogLevel::Default>(
            STR("[PalAccess]   {} {}\n"),
            prop->GetClass().GetName(), prop->GetName());
        // For struct properties whose name looks stat/save-shaped, also dump
        // the inner field list so we can see HP/SP locations.
        if (prop->GetClass().GetName() == L"StructProperty") {
            auto pname_lower = ToLower(prop->GetName());
            const bool interesting =
                pname_lower.find(L"save")      != std::wstring::npos ||
                pname_lower.find(L"parameter") != std::wstring::npos ||
                pname_lower == L"hp"   || pname_lower == L"maxhp"   ||
                pname_lower == L"sp"   || pname_lower == L"maxsp"   ||
                pname_lower == L"mp"   || pname_lower == L"maxmp"   ||
                pname_lower == L"shieldhp"   || pname_lower == L"shieldmaxhp" ||
                pname_lower == L"hunger"     || pname_lower == L"fullstomach" ||
                pname_lower == L"stamina"    || pname_lower == L"sanity";
            if (interesting) {
                auto* sp = reinterpret_cast<Unreal::FStructProperty*>(prop);
                std::wstring sub_label = std::wstring(label) + L"." + prop->GetName();
                DumpScriptStructInner(sp->GetStruct().Get(), sub_label);
            }
        }
    }
}

void DumpStatComponentsRecursive(Unreal::UObject* obj, const std::wstring& label,
                                 int depth, int max_depth,
                                 std::unordered_set<uintptr_t>& visited) {
    if (!obj || depth > max_depth) return;
    auto key = reinterpret_cast<uintptr_t>(obj);
    if (!visited.insert(key).second) return;

    DumpClass(obj, label.c_str());
    if (depth == max_depth) return;
    auto cls = obj->GetClassPrivate();
    if (!cls) return;
    for (auto* prop : cls->ForEachPropertyInChain()) {
        if (!prop) continue;
        if (prop->GetClass().GetName() != L"ObjectProperty") continue;
        auto pname = prop->GetName();
        if (!LooksLikeStatComponent(pname)) continue;
        auto** child_ptr = prop->ContainerPtrToValuePtr<Unreal::UObject*>(obj);
        if (!child_ptr || !*child_ptr) continue;
        std::wstring child_label = label + L"." + pname;
        DumpStatComponentsRecursive(*child_ptr, child_label, depth + 1, max_depth, visited);
    }
}

void DumpStatComponents(Unreal::UObject* obj, const wchar_t* who) {
    std::unordered_set<uintptr_t> visited;
    DumpStatComponentsRecursive(obj, std::wstring(who), 0, /*max_depth=*/2, visited);
}

void Diagnose() {
    EnsureCached();
    auto* p = CachedPlayer();
    auto* s = CachedPlayerState();
    if (!p && !s) {
        Speech::Get().Announce(L"Player not found. Walk around so the character spawns.");
        Output::send<LogLevel::Default>(STR("[PalAccess] F12 diagnose: nothing cached\n"));
        return;
    }
    std::wstring msg = L"Diagnose. ";
    if (p) {
        msg += L"Pawn ";
        msg += p->GetClassPrivate()->GetName();
        msg += L". ";
    }
    if (s) {
        msg += L"State ";
        msg += s->GetClassPrivate()->GetName();
        msg += L".";
    }
    Speech::Get().Announce(std::wstring_view(msg));
    DumpClass(p, L"player pawn");
    DumpClass(s, L"player state");
    DumpStatComponents(p, L"pawn");
    DumpStatComponents(s, L"state");

    // Probe the stat reader for each keyword we expose so it's obvious
    // whether values are being found at all.
    auto probe = [&](std::wstring_view label, std::wstring_view keyword) {
        auto pair = ReadStatPair(p, keyword);
        if (!pair.cur && s) pair = ReadStatPair(s, keyword);
        if (pair.cur || pair.max) {
            Output::send<LogLevel::Default>(
                STR("[PalAccess] probe '{}' cur={} max={}\n"),
                label,
                pair.cur ? std::to_wstring(*pair.cur) : L"<none>",
                pair.max ? std::to_wstring(*pair.max) : L"<none>");
        } else {
            Output::send<LogLevel::Default>(
                STR("[PalAccess] probe '{}' no match\n"), label);
        }
    };
    probe(L"hp",      L"hp");
    probe(L"sp",      L"sp");
    probe(L"hunger",  L"hunger");
    probe(L"stamina", L"stamina");
    probe(L"sanity",  L"sanity");
    probe(L"level",   L"level");
    probe(L"exp",     L"exp");
}

} // namespace

Unreal::UObject* Hotkeys::GetCachedPlayer() {
    return GetPlayer();  // validates + rescans if dead
}

void Hotkeys::NoticePotentialGaugeWidget(Unreal::UObject*) {
    // Kept as a no-op so Hooks.cpp can keep its call site stable.
    // The gauge-widget cache was removed because the menu's TextBlocks
    // hold stale values once the menu closes.
}

void Hotkeys::NoticePotentialPlayer(Unreal::UObject* actor) {
    if (!actor || IsDefaultObject(actor)) return;
    auto cls = actor->GetClassPrivate();
    if (!cls) return;
    auto cn = cls->GetName();
    if (LooksLikePlayer(cn)) {
        SetCachedPlayer(actor);
        Output::send<LogLevel::Default>(
            STR("[PalAccess] cached player pawn: {} ({})\n"), actor->GetName(), cn);
    } else if (LooksLikePlayerState(cn)) {
        SetCachedPlayerState(actor);
        Output::send<LogLevel::Default>(
            STR("[PalAccess] cached player state: {} ({})\n"), actor->GetName(), cn);
    }
}

void Hotkeys::Tick() {
    // Only react to keys/buttons when Palworld owns the foreground window.
    // GetAsyncKeyState + XInput both report state globally, so without this
    // gate alt-tabbing to another app and pressing Tab would still trigger
    // target mode, F-keys would still fire, etc.
    {
        DWORD fg_pid = 0;
        ::GetWindowThreadProcessId(::GetForegroundWindow(), &fg_pid);
        if (fg_pid != ::GetCurrentProcessId()) return;
    }

    struct Entry {
        int   vk;
        bool  prev;
        void (*action)();
    };
    static Entry table[] = {
        { VK_F1,  false, []{ SpeakStatBy(L"H P,",
                              { L"Hp" }, { L"MaxHP" },
                              L"H P not found, press F12 to diagnose."); } },
        { VK_F2,  false, []{ SpeakStatBy(L"Hunger,",
                              { L"FullStomach" }, { L"MaxFullStomach" },
                              L"Hunger not found."); } },
        { VK_F3,  false, []{ SpeakStatBy(L"Stamina,",
                              { L"SP", L"MP" }, { L"MaxSP", L"MaxMP" },
                              L"Stamina not found."); } },
        { VK_F4,  false, []{ SpeakStatBy(L"Level,",
                              { L"Level" }, {},
                              L"Level not found."); } },
        { VK_F5,  false, []{ Nav::ListNearby(); } },
        { VK_F11, false, []{
            // Controller diagnostic. DualSense / DualShock need Steam Input
            // (or DS4Windows) to appear as XInput. If no slot reports
            // connected, the user must enable Steam Input for Palworld.
            int count = 0;
            for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
                XINPUT_STATE xs{};
                if (XInputGetState(i, &xs) == ERROR_SUCCESS) ++count;
            }
            if (count == 0) {
                Speech::Get().Announce(
                    L"No X Input controller detected. In Steam, right-click Palworld, "
                    L"Properties, Controller, enable Steam Input.");
            } else {
                std::wstring msg = L"X Input controllers connected: ";
                msg += std::to_wstring(count);
                Speech::Get().Announce(std::wstring_view(msg));
            }
        }},
        { VK_F6,  false, []{ Nav::ToggleMenu(); } },
        { VK_F9,  false, []{ Nav::CancelArmedTarget(); } },
        { VK_F12, false, []{ Diagnose(); } },
        // Keyboard menu/target-lock navigation keys. Target-lock (Tab/LB
        // held) takes priority over the nav menu when both are active.
        { VK_UP,     false, []{
            if (Nav::IsTargetModeActive()) Nav::TargetPrev();
            else if (Nav::IsMenuOpen())    Nav::MenuPrev();
        }},
        { VK_DOWN,   false, []{
            if (Nav::IsTargetModeActive()) Nav::TargetNext();
            else if (Nav::IsMenuOpen())    Nav::MenuNext();
        }},
        { VK_LEFT,   false, []{
            if (Nav::IsTargetModeActive()) Nav::TargetCategoryPrev();
        }},
        { VK_RIGHT,  false, []{
            if (Nav::IsTargetModeActive()) Nav::TargetCategoryNext();
        }},
        { VK_RETURN, false, []{ if (Nav::IsMenuOpen()) Nav::MenuConfirm(); } },
        { VK_ESCAPE, false, []{ if (Nav::IsMenuOpen()) Nav::MenuClose(); } },
    };
    for (auto& e : table) {
        bool now = (::GetAsyncKeyState(e.vk) & 0x8000) != 0;
        if (now && !e.prev) e.action();
        e.prev = now;
    }

    // ---- Gamepad polling via XInput -------------------------------------
    // VK_GAMEPAD_* virtual keys only exist for Windows Store apps. Palworld
    // is a Win32 game so we must poll XInput directly for the controller.
    struct GamepadState {
        bool lb = false, a = false, b = false;
        bool dup = false, ddown = false, dleft = false, dright = false;
    };
    auto read_gamepad = []() -> GamepadState {
        GamepadState s{};
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
            XINPUT_STATE xs{};
            if (XInputGetState(i, &xs) != ERROR_SUCCESS) continue;
            auto b = xs.Gamepad.wButtons;
            s.lb     = (b & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
            s.a      = (b & XINPUT_GAMEPAD_A)             != 0;
            s.b      = (b & XINPUT_GAMEPAD_B)             != 0;
            s.dup    = (b & XINPUT_GAMEPAD_DPAD_UP)       != 0;
            s.ddown  = (b & XINPUT_GAMEPAD_DPAD_DOWN)     != 0;
            s.dleft  = (b & XINPUT_GAMEPAD_DPAD_LEFT)     != 0;
            s.dright = (b & XINPUT_GAMEPAD_DPAD_RIGHT)    != 0;
            break;
        }
        return s;
    };

    static GamepadState prev_gp{};
    GamepadState gp = read_gamepad();

    // Keyboard Tab mirrors LB so the same flow works without a controller.
    constexpr int kVkTabKey = 0x09;  // (VK_TAB is a windows.h macro)
    static bool prev_lb_or_tab = false;
    bool tab_held = (::GetAsyncKeyState(kVkTabKey) & 0x8000) != 0;
    bool lb_or_tab = gp.lb || tab_held;
    // Suppress target-lock entirely while an in-game menu (construction,
    // inventory, settings, etc.) is open — those menus typically use LB/RB
    // as tab-switch shortcuts and we don't want to fight them.
    if (Nav::IsInGameMenuOpen()) lb_or_tab = false;
    if (lb_or_tab && !prev_lb_or_tab)        Nav::EnterTargetMode();
    else if (!lb_or_tab && prev_lb_or_tab)   Nav::ExitTargetMode();
    prev_lb_or_tab = lb_or_tab;

    // D-pad: target-lock takes priority over nav menu.
    if (gp.dup && !prev_gp.dup) {
        if (Nav::IsTargetModeActive())  Nav::TargetPrev();
        else if (Nav::IsMenuOpen())     Nav::MenuPrev();
    }
    if (gp.ddown && !prev_gp.ddown) {
        if (Nav::IsTargetModeActive())  Nav::TargetNext();
        else if (Nav::IsMenuOpen())     Nav::MenuNext();
    }
    if (gp.dleft && !prev_gp.dleft && Nav::IsTargetModeActive()) {
        Nav::TargetCategoryPrev();
    }
    if (gp.dright && !prev_gp.dright && Nav::IsTargetModeActive()) {
        Nav::TargetCategoryNext();
    }
    if (gp.a && !prev_gp.a && Nav::IsMenuOpen()) Nav::MenuConfirm();
    if (gp.b && !prev_gp.b && Nav::IsMenuOpen()) Nav::MenuClose();
    prev_gp = gp;

    // Phase-3 directional guidance: drives periodic announcements while a
    // nav target is armed. No-op when nothing's armed.
    Nav::Tick();
}

} // namespace PalAccess
