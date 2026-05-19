#include "Nav.hpp"
#include "Hotkeys.hpp"
#include "Speech.hpp"

// UE4SS headers first to keep min/max macros from contaminating UE types.
#include <DynamicOutput/Output.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Constructs/Loop.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UScriptStruct.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string_view>
#include <unordered_set>
#include <vector>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmreg.h>
#include <xaudio2.h>

using namespace RC;

namespace PalAccess {

namespace {

bool IsDefaultObject(Unreal::UObject* obj) {
    return !obj || obj->GetName().starts_with(L"Default__");
}

std::wstring ClassName(Unreal::UObject* obj) {
    if (!obj) return {};
    auto* c = obj->GetClassPrivate();
    return c ? c->GetName() : std::wstring{};
}

// Read an FVector struct property at base+offset as 3 doubles. UE5's FVector
// is { double X, Y, Z }. The struct's reflected name is "Vector".
std::optional<std::array<double, 3>>
ReadFVectorAt(Unreal::FProperty* prop, uint8_t* base) {
    if (!prop || !base) return std::nullopt;
    if (prop->GetClass().GetName() != L"StructProperty") return std::nullopt;
    auto* sp = reinterpret_cast<Unreal::FStructProperty*>(prop);
    Unreal::UScriptStruct* st = sp->GetStruct().Get();
    if (!st) return std::nullopt;
    auto sn = st->GetName();
    if (sn != L"Vector" && sn != L"Vector3d" && sn != L"Vector3f") return std::nullopt;
    uint8_t* p = base + prop->GetOffset_Internal();
    if (sn == L"Vector3f") {
        return std::array<double, 3>{
            static_cast<double>(*reinterpret_cast<float*>(p)),
            static_cast<double>(*reinterpret_cast<float*>(p + 4)),
            static_cast<double>(*reinterpret_cast<float*>(p + 8))
        };
    }
    return std::array<double, 3>{
        *reinterpret_cast<double*>(p),
        *reinterpret_cast<double*>(p + 8),
        *reinterpret_cast<double*>(p + 16)
    };
}

// Map an actor class name to a (category, friendly_name) pair. Returns empty
// pair if the class isn't a POI we recognise.
struct PoiHint { std::wstring_view category; std::wstring display; };

std::wstring HumanizeRegion(std::wstring_view raw) {
    // "BP_DungeonEntrance_Forest_C" -> "Forest"
    std::wstring s(raw);
    // Strip "_C" suffix
    if (s.ends_with(L"_C")) s.resize(s.size() - 2);
    // Take the last underscore-separated token (often the region name)
    auto pos = s.rfind(L'_');
    if (pos != std::wstring::npos) return s.substr(pos + 1);
    return s;
}

std::wstring HumanizeTargetName(std::wstring_view cn) {
    std::wstring s(cn);
    if (s.starts_with(L"BP_")) s.erase(0, 3);
    if (s.ends_with(L"_C")) s.resize(s.size() - 2);
    if (s.ends_with(L"_Normal")) s.resize(s.size() - 7);
    std::replace(s.begin(), s.end(), L'_', L' ');
    return s;
}

// Classify an actor for target-lock. Returns the spoken category name
// ("Enemies", "Pals", "Trees", "Resources", "POIs", "Fishing") and a
// display name. Empty category = not a target.
struct TargetHint { std::wstring category; std::wstring display; };

TargetHint ClassifyTarget(std::wstring_view cn) {
    if (cn.empty()) return {};
    auto has = [&](std::wstring_view needle) {
        return cn.find(needle) != std::wstring::npos;
    };

    // Player isn't a target. Skip spawners (they're invisible).
    if (cn.starts_with(L"BP_Player_")) return {};

    // Resource spawners — categorise BEFORE the generic "Spawner" exclusion
    // would have hidden them.
    if (cn.starts_with(L"BP_PalMapObjectSpawner_log") || has(L"Tree")) {
        return { L"Trees", L"Tree" };
    }
    if (cn.starts_with(L"BP_PalMapObjectSpawner_")) {
        // "BP_PalMapObjectSpawner_RockCoal_C" → "Rock coal"
        constexpr std::wstring_view pfx = L"BP_PalMapObjectSpawner_";
        std::wstring tail(cn.substr(pfx.size()));
        if (tail.ends_with(L"_C")) tail.resize(tail.size() - 2);
        while (!tail.empty() && (std::iswdigit(tail.back()) || tail.back() == L'_')) {
            tail.pop_back();
        }
        std::replace(tail.begin(), tail.end(), L'_', L' ');
        if (tail.empty()) tail = L"Resource";
        else tail[0] = std::towupper(tail[0]);
        return { L"Resources", std::move(tail) };
    }

    // POIs (dungeons, arenas, fishing spots, fast travel)
    if (cn.starts_with(L"BP_DungeonEntrance_") ||
        cn.starts_with(L"BP_DungeonFixedEntrance_") ||
        cn == L"BP_ArenaEntrance_C" ||
        cn == L"BP_LevelObject_StaticRespawnPoint_C" ||
        has(L"FastTravel") || has(L"GreatEagle") ||
        has(L"SyndicateTower") || has(L"TowerBoss")) {
        if (cn.starts_with(L"BP_FishingSpot_")) {
            return { L"Fishing", has(L"Rare") ? L"Rare fishing spot" : L"Fishing spot" };
        }
        return { L"POIs", HumanizeTargetName(cn) };
    }
    if (cn.starts_with(L"BP_FishingSpot_")) {
        return { L"Fishing", has(L"Rare") ? L"Rare fishing spot" : L"Fishing spot" };
    }

    // Hard exclusions for animated/character classes only.
    if (has(L"Widget") || has(L"Component") || has(L"HUD") ||
        has(L"Manager")|| has(L"Volume")    || has(L"Marker") ||
        has(L"Camera") || has(L"Light")     || has(L"Controller")) {
        return {};
    }

    // Enemies (hostile humanoid NPCs).
    if (has(L"_NPC") || has(L"NPC_")) {
        return { L"Enemies", HumanizeTargetName(cn) };
    }
    if (has(L"_Boss") || has(L"Boss_")) {
        return { L"Enemies", HumanizeTargetName(cn) };
    }

    // Pals (wild and friendly characters).
    if (has(L"_Pal") || has(L"Pal_") || cn.ends_with(L"_Normal_C")) {
        return { L"Pals", HumanizeTargetName(cn) };
    }
    return {};
}

PoiHint ClassifyPOI(std::wstring_view cn) {
    if (cn.starts_with(L"BP_DungeonEntrance_")) {
        return { L"Dungeon", HumanizeRegion(cn) + L" dungeon" };
    }
    if (cn.starts_with(L"BP_DungeonFixedEntrance_")) {
        // "BP_DungeonFixedEntrance_forest_1_C" → "Forest 1 dungeon"
        std::wstring stripped(cn);
        if (stripped.ends_with(L"_C")) stripped.resize(stripped.size() - 2);
        constexpr std::wstring_view prefix = L"BP_DungeonFixedEntrance_";
        std::wstring tail = stripped.substr(prefix.size());
        std::replace(tail.begin(), tail.end(), L'_', L' ');
        if (!tail.empty()) tail[0] = std::towupper(tail[0]);
        return { L"Fixed dungeon", std::move(tail) + L" fixed dungeon" };
    }
    if (cn == L"BP_ArenaEntrance_C") {
        return { L"Arena", L"Arena entrance" };
    }
    if (cn.starts_with(L"BP_FishingSpot_")) {
        std::wstring rarity = std::wstring(cn).find(L"Rare") != std::wstring::npos
            ? L"Rare fishing spot" : L"Fishing spot";
        return { L"Fishing", rarity };
    }
    if (cn == L"BP_LevelObject_StaticRespawnPoint_C") {
        return { L"Respawn", L"Respawn point" };
    }
    if (cn.find(L"FastTravel") != std::wstring::npos ||
        cn.find(L"GreatEagle") != std::wstring::npos) {
        return { L"Fast travel", L"Fast travel statue" };
    }
    if (cn.find(L"SyndicateTower") != std::wstring::npos ||
        cn.find(L"TowerBoss")      != std::wstring::npos) {
        return { L"Tower", L"Syndicate tower" };
    }
    if (cn == L"BP_OilrigNPCSpawner_Mono_C") {
        return { L"Oilrig", L"Oil rig" };
    }
    // Resource spawners — trees, stones, ore. These dot the open world and
    // can be hundreds of them, so we'll cap the menu length below.
    if (cn.starts_with(L"BP_PalMapObjectSpawner_log") ||
        cn.find(L"Tree") != std::wstring::npos) {
        return { L"Tree", L"Tree" };
    }
    return {};
}

std::wstring FormatDistance(double cm) {
    double meters = cm / 100.0;
    wchar_t buf[64];
    if (meters >= 1000.0) {
        std::swprintf(buf, 64, L"%.1f kilometers", meters / 1000.0);
    } else {
        std::swprintf(buf, 64, L"%.0f meters", std::round(meters));
    }
    return buf;
}

// Menu / armed-target / target-lock state shared between hotkey handlers.
struct NavState {
    std::mutex                       mtx;
    bool                             menu_open = false;
    std::vector<Nav::POI>            menu_list;
    std::size_t                      selected_index = 0;
    std::optional<Nav::POI>          armed_target;

    // Target-lock (LB held): nearby things grouped by category. D-pad
    // left/right cycles category; up/down cycles within. On release, the
    // currently highlighted item is promoted to armed_target.
    bool                             target_mode = false;
    struct CategoryGroup { std::wstring name; std::vector<Nav::POI> items; };
    std::vector<CategoryGroup>       target_groups;
    std::size_t                      cat_index    = 0;
    std::size_t                      item_index   = 0;
};
NavState g_state;

// Shared with Hotkeys' validated cache so we don't fail to find the player
// even if a fresh UObject scan would miss it for some reason.
Unreal::UObject* GetPlayerCached() {
    return Hotkeys::GetCachedPlayer();
}

// Returns the player position with a more informative reason on failure
// (no pawn vs no location), and logs so we can diagnose if it breaks again.
struct PlayerPosResult {
    std::optional<std::array<double, 3>> pos;
    std::wstring_view error;  // populated when pos is empty
};

PlayerPosResult GetPlayerPosOrError() {
    auto* p = GetPlayerCached();
    if (!p) {
        Output::send<LogLevel::Verbose>(STR("[PalAccess] Nav: player pawn not cached\n"));
        return { std::nullopt, L"Player not loaded." };
    }
    auto loc = Nav::GetActorLocation(p);
    if (!loc) {
        Output::send<LogLevel::Verbose>(
            STR("[PalAccess] Nav: K2_GetActorLocation failed on {} ({})\n"),
            p->GetName(), p->GetClassPrivate() ? p->GetClassPrivate()->GetName() : L"<null>");
        return { std::nullopt, L"Could not read player position." };
    }
    return { loc, {} };
}

std::optional<std::array<double, 3>> GetPlayerLocation() {
    return GetPlayerPosOrError().pos;
}

// Announce the currently selected entry of the open menu.
void AnnounceSelection() {
    auto& s = g_state;
    if (s.menu_list.empty()) return;
    const auto& poi = s.menu_list[s.selected_index];
    std::wstring msg = std::to_wstring(s.selected_index + 1);
    msg += L" of ";
    msg += std::to_wstring(s.menu_list.size());
    msg += L". ";
    msg += poi.name;
    if (auto pp = GetPlayerLocation()) {
        auto d = Nav::DistanceCm(poi, (*pp)[0], (*pp)[1], (*pp)[2]);
        msg += L", ";
        msg += FormatDistance(d);
    }
    Speech::Get().FocusUpdate(std::wstring_view(msg));
}

} // namespace

std::optional<std::array<double, 3>>
Nav::GetActorLocation(Unreal::UObject* actor) {
    if (!actor) return std::nullopt;

    // Preferred path: call AActor::K2_GetActorLocation(). UE5 hides
    // SceneComponent.RelativeLocation behind a MemberVariableLayout wrapper
    // that ForEachPropertyInChain doesn't enumerate, so direct reflection
    // would miss it. Calling the blueprint-callable getter is what every
    // blueprint does to read this — it's safe and authoritative.
    if (auto* func = actor->GetFunctionByName(L"K2_GetActorLocation")) {
        struct Parms { double X, Y, Z; };
        Parms parms{};
        actor->ProcessEvent(func, &parms);
        return std::array<double, 3>{ parms.X, parms.Y, parms.Z };
    }

    // Fallback: try reflection on RootComponent.RelativeLocation in case
    // GetFunctionByName didn't resolve.
    auto* cls = actor->GetClassPrivate();
    if (!cls) return std::nullopt;
    for (auto* prop : cls->ForEachPropertyInChain()) {
        if (!prop) continue;
        if (prop->GetName() != L"RootComponent") continue;
        if (prop->GetClass().GetName() != L"ObjectProperty") continue;
        auto** rc_ptr = prop->ContainerPtrToValuePtr<Unreal::UObject*>(actor);
        if (!rc_ptr || !*rc_ptr) return std::nullopt;
        auto* root = *rc_ptr;
        auto* rc_cls = root->GetClassPrivate();
        if (!rc_cls) return std::nullopt;
        for (auto* rprop : rc_cls->ForEachPropertyInChain()) {
            if (!rprop) continue;
            if (rprop->GetName() != L"RelativeLocation") continue;
            if (auto v = ReadFVectorAt(rprop, reinterpret_cast<uint8_t*>(root))) {
                return v;
            }
        }
    }
    return std::nullopt;
}

std::vector<Nav::POI> Nav::ScanPOIs() {
    std::vector<POI> out;
    std::unordered_set<std::wstring> seen_class_log;

    Unreal::UObjectGlobals::ForEachUObject(
        [&](Unreal::UObject* obj, int32_t, int32_t) -> LoopAction {
            if (IsDefaultObject(obj)) return LoopAction::Continue;
            auto cn = ClassName(obj);
            if (cn.empty()) return LoopAction::Continue;
            auto hint = ClassifyPOI(cn);
            if (hint.category.empty()) return LoopAction::Continue;
            auto loc = GetActorLocation(obj);
            if (!loc) return LoopAction::Continue;
            POI p;
            p.name     = std::wstring(hint.display);
            p.category = std::wstring(hint.category);
            p.x = (*loc)[0]; p.y = (*loc)[1]; p.z = (*loc)[2];
            out.push_back(std::move(p));
            return LoopAction::Continue;
        });
    return out;
}

double Nav::DistanceCm(const POI& a, double px, double py, double pz) {
    double dx = a.x - px, dy = a.y - py, dz = a.z - pz;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void Nav::ListNearby() {
    auto pr = GetPlayerPosOrError();
    if (!pr.pos) {
        Speech::Get().Announce(pr.error);
        return;
    }
    auto& pp = pr.pos;

    auto pois = ScanPOIs();
    if (pois.empty()) {
        Speech::Get().Announce(L"No points of interest found nearby.");
        return;
    }
    std::sort(pois.begin(), pois.end(), [&](const POI& a, const POI& b) {
        return DistanceCm(a, (*pp)[0], (*pp)[1], (*pp)[2]) <
               DistanceCm(b, (*pp)[0], (*pp)[1], (*pp)[2]);
    });

    Output::send<LogLevel::Default>(
        STR("[PalAccess] Nav scan found {} POIs\n"), static_cast<int>(pois.size()));

    std::wstring msg = L"Nearby points of interest. ";
    int announced = 0;
    for (auto& p : pois) {
        if (announced >= 5) break;
        auto dist = DistanceCm(p, (*pp)[0], (*pp)[1], (*pp)[2]);
        msg += p.name;
        msg += L", ";
        msg += FormatDistance(dist);
        msg += L". ";
        ++announced;
    }
    Speech::Get().Announce(std::wstring_view(msg));
}

// ---- Phase 2: selection menu --------------------------------------------

void Nav::ToggleMenu() {
    std::lock_guard<std::mutex> lock(g_state.mtx);
    if (g_state.menu_open) {
        g_state.menu_open = false;
        Speech::Get().Announce(L"Navigation menu closed.");
        return;
    }
    auto pr = GetPlayerPosOrError();
    if (!pr.pos) {
        Speech::Get().Announce(pr.error);
        return;
    }
    auto& pp = pr.pos;
    auto pois = ScanPOIs();
    if (pois.empty()) {
        Speech::Get().Announce(L"No points of interest found.");
        return;
    }
    std::sort(pois.begin(), pois.end(), [&](const POI& a, const POI& b) {
        return DistanceCm(a, (*pp)[0], (*pp)[1], (*pp)[2]) <
               DistanceCm(b, (*pp)[0], (*pp)[1], (*pp)[2]);
    });
    // Cap to the 50 closest so the user doesn't have to cycle past every
    // tree on the map. Tweak this if it feels too short or too long.
    constexpr std::size_t kMaxEntries = 50;
    if (pois.size() > kMaxEntries) pois.resize(kMaxEntries);
    g_state.menu_list = std::move(pois);
    g_state.selected_index = 0;
    g_state.menu_open = true;
    Speech::Get().Announce(
        L"Navigation menu open. Arrow keys to navigate, Enter to select, escape or F6 to close.");
    AnnounceSelection();
}

void Nav::MenuNext() {
    std::lock_guard<std::mutex> lock(g_state.mtx);
    if (!g_state.menu_open || g_state.menu_list.empty()) return;
    g_state.selected_index =
        (g_state.selected_index + 1) % g_state.menu_list.size();
    AnnounceSelection();
}

void Nav::MenuPrev() {
    std::lock_guard<std::mutex> lock(g_state.mtx);
    if (!g_state.menu_open || g_state.menu_list.empty()) return;
    g_state.selected_index =
        (g_state.selected_index + g_state.menu_list.size() - 1) % g_state.menu_list.size();
    AnnounceSelection();
}

void Nav::MenuConfirm() {
    std::lock_guard<std::mutex> lock(g_state.mtx);
    if (!g_state.menu_open || g_state.menu_list.empty()) return;
    g_state.armed_target = g_state.menu_list[g_state.selected_index];
    g_state.menu_open = false;
    std::wstring msg = L"Tracking ";
    msg += g_state.armed_target->name;
    msg += L".";
    Speech::Get().Announce(std::wstring_view(msg));
}

void Nav::MenuClose() {
    std::lock_guard<std::mutex> lock(g_state.mtx);
    if (!g_state.menu_open) return;
    g_state.menu_open = false;
    Speech::Get().Announce(L"Navigation menu closed.");
}

void Nav::CancelArmedTarget() {
    std::lock_guard<std::mutex> lock(g_state.mtx);
    if (!g_state.armed_target) return;
    g_state.armed_target.reset();
    // g_last_direction is in the phase-3 anonymous namespace below; resetting
    // it isn't needed for correctness — once armed_target is gone, the tick
    // exits early and won't speak a stale direction.
    Speech::Get().Announce(L"Tracking cancelled.");
}

bool Nav::IsMenuOpen() {
    std::lock_guard<std::mutex> lock(g_state.mtx);
    return g_state.menu_open;
}

bool Nav::IsInGameMenuOpen() {
    // Scan the live UObject array for any in-game / pause menu widget that
    // is currently alive. This tells us whether the user is interacting
    // with Palworld's native UI (Construction, Inventory, etc.) so we can
    // get out of the way of its native tab-switching inputs.
    static constexpr std::wstring_view kMenuClasses[] = {
        L"WBP_InGameMainMenu_C",
        L"WBP_IngameMainMenu_C",
        L"WBP_MainMenu_C",
        L"WBP_PalInGameMainMenu_C",
        L"WBP_OptionSettings_C",
        L"WBP_Title_WorldSettings_C",
    };
    bool found = false;
    Unreal::UObjectGlobals::ForEachUObject(
        [&](Unreal::UObject* obj, int32_t, int32_t) -> LoopAction {
            if (found || !obj) return LoopAction::Continue;
            if (IsDefaultObject(obj)) return LoopAction::Continue;
            auto cn = ClassName(obj);
            for (auto m : kMenuClasses) {
                if (cn == m) { found = true; return LoopAction::Break; }
            }
            return LoopAction::Continue;
        });
    return found;
}

bool Nav::HasArmedTarget() {
    std::lock_guard<std::mutex> lock(g_state.mtx);
    return g_state.armed_target.has_value();
}

Nav::POI Nav::GetArmedTarget() {
    std::lock_guard<std::mutex> lock(g_state.mtx);
    return g_state.armed_target.value_or(POI{});
}

// ---- Phase 3: audio directional guidance --------------------------------

namespace {

constexpr double kPi      = 3.14159265358979323846;
constexpr double kDegPerR = 180.0 / kPi;

double Norm180(double deg) {
    while (deg >   180.0) deg -= 360.0;
    while (deg <= -180.0) deg += 360.0;
    return deg;
}

// Read PlayerCameraYaw FloatProperty (degrees, 0 = +X world axis).
std::optional<double> ReadPlayerYaw(Unreal::UObject* player) {
    if (!player) return std::nullopt;
    auto* cls = player->GetClassPrivate();
    if (!cls) return std::nullopt;
    for (auto* prop : cls->ForEachPropertyInChain()) {
        if (!prop) continue;
        if (prop->GetName() != L"PlayerCameraYaw") continue;
        if (prop->GetClass().GetName() != L"FloatProperty") continue;
        auto* v = prop->ContainerPtrToValuePtr<float>(player);
        if (v) return static_cast<double>(*v);
    }
    return std::nullopt;
}

std::wstring_view DirectionPhrase(double rel_deg) {
    double a = std::abs(rel_deg);
    if (a <= 22.5)  return L"ahead";
    if (a <= 67.5)  return rel_deg > 0 ? L"ahead and to your right" : L"ahead and to your left";
    if (a <= 112.5) return rel_deg > 0 ? L"to your right"           : L"to your left";
    if (a <= 157.5) return rel_deg > 0 ? L"behind and to your right": L"behind and to your left";
    return L"behind you";
}

// Arrival uses 3D distance so a target on a hill doesn't "arrive" early.
constexpr double kArrivalCm = 200.0;

// Audio beacon — locates navbeakon.wav at the game root and plays it at a
// distance-scaled rate (faster pings = closer).
std::wstring FindBeaconPath() {
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return {};
    std::filesystem::path p(exe);
    // exe at <game>/Pal/Binaries/Win64/Palworld-Win64-Shipping.exe
    if (p.has_parent_path()) p = p.parent_path();
    if (p.has_parent_path()) p = p.parent_path();
    if (p.has_parent_path()) p = p.parent_path();
    if (p.has_parent_path()) p = p.parent_path();
    p /= L"navbeakon.wav";
    return p.wstring();
}

const std::wstring& BeaconPath() {
    static const std::wstring p = []() {
        auto s = FindBeaconPath();
        if (s.empty()) {
            Output::send<LogLevel::Warning>(STR("[PalAccess] beacon path resolve failed\n"));
        } else if (!std::filesystem::exists(s)) {
            Output::send<LogLevel::Warning>(
                STR("[PalAccess] beacon WAV not found: {}\n"), s);
        } else {
            Output::send<LogLevel::Default>(STR("[PalAccess] beacon WAV: {}\n"), s);
        }
        return s;
    }();
    return p;
}

// ------- XAudio2 beacon with stereo pan + pitch -------------------------
//
// Pan : -1 = hard left, 0 = center, +1 = hard right (set from horizontal
//       bearing). Behind reuses the front mirror with reduced volume so
//       it still hints which side.
// Pitch: semitones, positive = higher (target is up / needs climbing).
class BeaconPlayer {
public:
    bool EnsureLoaded() {
        if (m_loaded)        return true;
        if (m_load_failed)   return false;
        m_load_failed = !Load();
        m_loaded      = !m_load_failed;
        return m_loaded;
    }

    void Play(float pan, float pitch_semi, float volume) {
        if (!EnsureLoaded() || !m_source) return;

        m_source->Stop();
        m_source->FlushSourceBuffers();

        XAUDIO2_BUFFER buf{};
        buf.pAudioData = m_pcm.data();
        buf.AudioBytes = static_cast<UINT32>(m_pcm.size());
        buf.Flags      = XAUDIO2_END_OF_STREAM;
        m_source->SubmitSourceBuffer(&buf);

        // Output matrix sized to master's actual channel count so we don't
        // clobber surround channels on 5.1 / 7.1 setups.
        XAUDIO2_VOICE_DETAILS md{};
        m_master->GetVoiceDetails(&md);
        UINT32 dst = md.InputChannels;
        UINT32 src = m_fmt.nChannels;
        std::vector<float> matrix(static_cast<size_t>(src) * dst, 0.0f);

        const double kPiHalf = 1.57079632679;
        float angle  = static_cast<float>((static_cast<double>(pan) + 1.0) * 0.5 * kPiHalf);
        float gainL  = std::cos(angle) * volume;
        float gainR  = std::sin(angle) * volume;

        if (src == 1 && dst >= 2) {
            // mono → stereo (first two output channels)
            matrix[0] = gainL;   // src0 → dstL
            matrix[1] = gainR;   // src0 → dstR
        } else if (src == 2 && dst >= 2) {
            // stereo → stereo with balance pan
            float l_keep = (pan <= 0) ? 1.0f : (1.0f - pan);
            float r_keep = (pan >= 0) ? 1.0f : (1.0f + pan);
            matrix[0]               = l_keep * volume; // L→L
            matrix[1]               = 0.0f;            // R→L
            matrix[dst + 0]         = 0.0f;            // L→R
            matrix[dst + 1]         = r_keep * volume; // R→R
        } else {
            // fallback: equal mix to L/R
            for (UINT32 c = 0; c < src; ++c) {
                matrix[c * dst + 0] = volume / src;
                if (dst >= 2) matrix[c * dst + 1] = volume / src;
            }
        }
        m_source->SetOutputMatrix(m_master, src, dst, matrix.data());

        // Pitch: clamp to ±1 octave so even big climbs sound musical.
        float ratio = std::pow(2.0f, std::clamp(pitch_semi, -12.0f, 12.0f) / 12.0f);
        if (ratio < XAUDIO2_MIN_FREQ_RATIO) ratio = XAUDIO2_MIN_FREQ_RATIO;
        if (ratio > 8.0f) ratio = 8.0f;
        m_source->SetFrequencyRatio(ratio);

        m_source->Start();
    }

private:
    bool                       m_loaded      = false;
    bool                       m_load_failed = false;
    IXAudio2*                  m_engine = nullptr;
    IXAudio2MasteringVoice*    m_master = nullptr;
    IXAudio2SourceVoice*       m_source = nullptr;
    std::vector<uint8_t>       m_pcm;
    WAVEFORMATEX               m_fmt{};

    bool Load() {
        const auto& path = BeaconPath();
        if (path.empty() || !std::filesystem::exists(path)) return false;
        if (!ReadWAV(path)) {
            Output::send<LogLevel::Warning>(STR("[PalAccess] failed to parse {}\n"), path);
            return false;
        }
        HRESULT hr = XAudio2Create(&m_engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (FAILED(hr)) return false;
        hr = m_engine->CreateMasteringVoice(&m_master);
        if (FAILED(hr)) return false;
        hr = m_engine->CreateSourceVoice(&m_source, &m_fmt,
                                         0, XAUDIO2_DEFAULT_FREQ_RATIO);
        if (FAILED(hr)) return false;
        Output::send<LogLevel::Default>(
            STR("[PalAccess] XAudio2 beacon ready ({} ch, {} Hz)\n"),
            (int)m_fmt.nChannels, (int)m_fmt.nSamplesPerSec);
        return true;
    }

    bool ReadWAV(const std::wstring& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        char riff[4], wave[4];
        uint32_t total_size = 0;
        f.read(riff, 4);
        f.read(reinterpret_cast<char*>(&total_size), 4);
        f.read(wave, 4);
        if (std::memcmp(riff, "RIFF", 4) != 0 ||
            std::memcmp(wave, "WAVE", 4) != 0) return false;
        bool fmt_ok = false, data_ok = false;
        while (f && !(fmt_ok && data_ok)) {
            char id[4];
            uint32_t csize = 0;
            f.read(id, 4);
            f.read(reinterpret_cast<char*>(&csize), 4);
            if (!f) break;
            if (std::memcmp(id, "fmt ", 4) == 0) {
                uint32_t to_read = std::min<uint32_t>(csize, sizeof(WAVEFORMATEX));
                f.read(reinterpret_cast<char*>(&m_fmt), to_read);
                if (csize > to_read) f.seekg(csize - to_read, std::ios::cur);
                fmt_ok = true;
            } else if (std::memcmp(id, "data", 4) == 0) {
                m_pcm.resize(csize);
                f.read(reinterpret_cast<char*>(m_pcm.data()), csize);
                data_ok = true;
            } else {
                f.seekg(csize, std::ios::cur);
            }
            if (csize & 1) f.seekg(1, std::ios::cur); // pad to even
        }
        return fmt_ok && data_ok && !m_pcm.empty();
    }
};

BeaconPlayer& Beacon() { static BeaconPlayer b; return b; }

void PlayBeacon(float pan = 0.0f, float pitch_semi = 0.0f, float volume = 1.0f) {
    Beacon().Play(pan, pitch_semi, volume);
}

// Distance → ping interval. Tight cadence when close, gentle when far.
// Roughly: 0 m ≈ 150 ms, 10 m ≈ 350 ms, 50 m ≈ 800 ms, 100 m ≈ 1300 ms,
// 200 m+ ≈ 2000 ms (capped).
std::chrono::milliseconds PingIntervalFor(double dist_cm) {
    double m = dist_cm / 100.0;
    double ms = 150.0 + m * 10.0;
    if (ms > 2000.0) ms = 2000.0;
    return std::chrono::milliseconds(static_cast<long long>(ms));
}

std::chrono::steady_clock::time_point g_last_ping{};
std::wstring                          g_last_direction;

} // namespace

// ---- Phase 4: target-lock mode ------------------------------------------

namespace {

constexpr double kTargetScanRadiusCm = 5000.0; // 50 m

// Order in which categories cycle via D-pad left/right. Empty groups are
// skipped at scan time so the user never lands on a vacuous category.
constexpr std::wstring_view kCategoryOrder[] = {
    L"Enemies", L"Pals", L"Trees", L"Resources", L"Fishing", L"POIs"
};

// Scan within radius; group by category in the canonical order; drop empties.
std::vector<NavState::CategoryGroup>
ScanTargetGroups(double px, double py, double pz) {
    std::unordered_map<std::wstring, std::vector<Nav::POI>> by_cat;
    Unreal::UObjectGlobals::ForEachUObject(
        [&](Unreal::UObject* obj, int32_t, int32_t) -> LoopAction {
            if (IsDefaultObject(obj)) return LoopAction::Continue;
            auto cn = ClassName(obj);
            auto hint = ClassifyTarget(cn);
            if (hint.category.empty()) return LoopAction::Continue;
            auto loc = Nav::GetActorLocation(obj);
            if (!loc) return LoopAction::Continue;
            double dx = (*loc)[0] - px, dy = (*loc)[1] - py, dz = (*loc)[2] - pz;
            double d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d > kTargetScanRadiusCm) return LoopAction::Continue;
            Nav::POI p;
            p.name     = std::move(hint.display);
            p.category = hint.category;
            p.x = (*loc)[0]; p.y = (*loc)[1]; p.z = (*loc)[2];
            by_cat[hint.category].push_back(std::move(p));
            return LoopAction::Continue;
        });

    std::vector<NavState::CategoryGroup> out;
    for (auto cat : kCategoryOrder) {
        auto it = by_cat.find(std::wstring(cat));
        if (it == by_cat.end() || it->second.empty()) continue;
        std::sort(it->second.begin(), it->second.end(),
                  [&](const Nav::POI& a, const Nav::POI& b) {
                      return Nav::DistanceCm(a, px, py, pz) <
                             Nav::DistanceCm(b, px, py, pz);
                  });
        out.push_back({ std::wstring(cat), std::move(it->second) });
    }
    return out;
}

// Speak the current selection: "Trees. 1 of 12. Tree, 35 meters."
void AnnounceTargetSelection() {
    auto& s = g_state;
    if (s.target_groups.empty()) return;
    if (s.cat_index >= s.target_groups.size()) return;
    auto& g = s.target_groups[s.cat_index];
    if (g.items.empty() || s.item_index >= g.items.size()) return;
    const auto& t = g.items[s.item_index];
    std::wstring msg = g.name;
    msg += L". ";
    msg += std::to_wstring(s.item_index + 1);
    msg += L" of ";
    msg += std::to_wstring(g.items.size());
    msg += L". ";
    msg += t.name;
    if (auto pp = GetPlayerLocation()) {
        auto d = Nav::DistanceCm(t, (*pp)[0], (*pp)[1], (*pp)[2]);
        msg += L", ";
        msg += FormatDistance(d);
    }
    Speech::Get().FocusUpdate(std::wstring_view(msg));
}

} // namespace

void Nav::EnterTargetMode() {
    auto pp = GetPlayerLocation();
    if (!pp) {
        Speech::Get().Announce(L"Cannot target, player not loaded.");
        return;
    }
    auto groups = ScanTargetGroups((*pp)[0], (*pp)[1], (*pp)[2]);
    if (groups.empty()) {
        Speech::Get().Announce(L"Nothing nearby to target.");
        return;
    }
    // Cap each category so we don't end up cycling 200 trees.
    constexpr std::size_t kMaxPerCat = 20;
    for (auto& g : groups) {
        if (g.items.size() > kMaxPerCat) g.items.resize(kMaxPerCat);
    }
    {
        std::lock_guard<std::mutex> lock(g_state.mtx);
        g_state.target_groups = std::move(groups);
        g_state.cat_index   = 0;
        g_state.item_index  = 0;
        g_state.target_mode = true;
    }
    AnnounceTargetSelection();
}

void Nav::ExitTargetMode() {
    POI armed{};
    bool had = false;
    {
        std::lock_guard<std::mutex> lock(g_state.mtx);
        if (!g_state.target_mode) return;
        g_state.target_mode = false;
        if (!g_state.target_groups.empty() &&
            g_state.cat_index < g_state.target_groups.size()) {
            auto& g = g_state.target_groups[g_state.cat_index];
            if (!g.items.empty() && g_state.item_index < g.items.size()) {
                armed = g.items[g_state.item_index];
                g_state.armed_target = armed;
                had = true;
            }
        }
        g_state.target_groups.clear();
    }
    if (had) {
        std::wstring msg = L"Targeting ";
        msg += armed.name;
        msg += L".";
        Speech::Get().Announce(std::wstring_view(msg));
        // Reset the guidance state so the first tick announces direction
        // and pings the beacon immediately.
        g_last_direction.clear();
        g_last_ping = std::chrono::steady_clock::time_point{};
    }
}

void Nav::TargetNext() {
    std::lock_guard<std::mutex> lock(g_state.mtx);
    if (!g_state.target_mode || g_state.target_groups.empty()) return;
    auto& g = g_state.target_groups[g_state.cat_index];
    if (g.items.empty()) return;
    g_state.item_index = (g_state.item_index + 1) % g.items.size();
    AnnounceTargetSelection();
}

void Nav::TargetPrev() {
    std::lock_guard<std::mutex> lock(g_state.mtx);
    if (!g_state.target_mode || g_state.target_groups.empty()) return;
    auto& g = g_state.target_groups[g_state.cat_index];
    if (g.items.empty()) return;
    g_state.item_index = (g_state.item_index + g.items.size() - 1) % g.items.size();
    AnnounceTargetSelection();
}

void Nav::TargetCategoryNext() {
    std::lock_guard<std::mutex> lock(g_state.mtx);
    if (!g_state.target_mode || g_state.target_groups.empty()) return;
    g_state.cat_index = (g_state.cat_index + 1) % g_state.target_groups.size();
    g_state.item_index = 0;
    AnnounceTargetSelection();
}

void Nav::TargetCategoryPrev() {
    std::lock_guard<std::mutex> lock(g_state.mtx);
    if (!g_state.target_mode || g_state.target_groups.empty()) return;
    g_state.cat_index =
        (g_state.cat_index + g_state.target_groups.size() - 1) % g_state.target_groups.size();
    g_state.item_index = 0;
    AnnounceTargetSelection();
}

bool Nav::IsTargetModeActive() {
    std::lock_guard<std::mutex> lock(g_state.mtx);
    return g_state.target_mode;
}

// -------------------------------------------------------------------------

void Nav::Tick() {
    if (!HasArmedTarget()) return;
    auto* player = Hotkeys::GetCachedPlayer();
    if (!player) return;
    auto pp = GetActorLocation(player);
    if (!pp) return;
    auto target = GetArmedTarget();
    auto now = std::chrono::steady_clock::now();

    double dx = target.x - (*pp)[0];
    double dy = target.y - (*pp)[1];
    double dz = target.z - (*pp)[2];
    double dist_2d = std::sqrt(dx * dx + dy * dy);
    double dist_3d = std::sqrt(dx * dx + dy * dy + dz * dz);

    if (dist_3d <= kArrivalCm) {
        std::wstring msg = L"Arrived at " + target.name + L".";
        Speech::Get().Announce(std::wstring_view(msg));
        std::lock_guard<std::mutex> lock(g_state.mtx);
        g_state.armed_target.reset();
        g_last_direction.clear();
        return;
    }

    // Compute pan/pitch from geometry. Yaw is needed for pan; if we can't
    // read it, fall back to centered audio.
    float pan         = 0.0f;
    float pitch_semi  = 0.0f;
    float volume      = 1.0f;
    std::wstring spoken_direction;
    if (auto yaw = ReadPlayerYaw(player)) {
        double bearing = std::atan2(dy, dx) * kDegPerR;
        double rel     = Norm180(bearing - *yaw);
        const double kPi180 = kPi / 180.0;
        if (std::abs(rel) <= 90.0) {
            pan = static_cast<float>(std::sin(rel * kPi180));
        } else {
            // Behind: mirror the bearing to the front for L/R hint, drop
            // volume so the user can tell it's behind them.
            double mirror = (rel > 0 ? 180.0 - rel : -180.0 - rel);
            pan    = static_cast<float>(std::sin(mirror * kPi180));
            volume = 0.55f;
        }
        spoken_direction = std::wstring(DirectionPhrase(rel));
    }
    // Pitch: 1 octave per ~24 m of vertical climb. Positive dz = target
    // above (climb up). Clamped to ±12 semitones (±1 octave).
    {
        float dz_m = static_cast<float>(dz / 100.0);
        pitch_semi = std::clamp(dz_m / 2.0f, -12.0f, 12.0f);
    }

    if (now - g_last_ping >= PingIntervalFor(dist_3d)) {
        PlayBeacon(pan, pitch_semi, volume);
        g_last_ping = now;
    }

    // Direction is only spoken when it CHANGES — beacon pan handles the
    // continuous "where" feedback, TTS is just for category transitions.
    if (!spoken_direction.empty() && spoken_direction != g_last_direction) {
        Speech::Get().FocusUpdate(std::wstring_view(spoken_direction));
        g_last_direction = std::move(spoken_direction);
    }
}

} // namespace PalAccess
