#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace RC::Unreal { class UObject; }

namespace PalAccess {

// World-location-aware POI navigation. Phase 1: discover and list. Later
// phases add selection + directional audio guidance.
class Nav {
public:
    struct POI {
        std::wstring name;
        std::wstring category;
        double       x = 0, y = 0, z = 0;
    };

    // Scan the live UObject array for POI actors (dungeons, arenas, fast
    // travel statues, syndicate towers, etc.) and return them with world
    // positions read from RootComponent.RelativeLocation.
    static std::vector<POI> ScanPOIs();

    // Read an actor's world position via its RootComponent. Returns nullopt
    // if no RootComponent or no RelativeLocation field is reachable.
    static std::optional<std::array<double, 3>> GetActorLocation(RC::Unreal::UObject* actor);

    // Euclidean distance in centimeters between (a) and (px,py,pz).
    static double DistanceCm(const POI& a, double px, double py, double pz);

    // F5 handler: announce the closest 5 POIs with distance in meters.
    static void ListNearby();

    // Selection menu (phase 2):
    //   F6                          — toggle menu open/closed
    //   Up arrow / D-pad up         — previous entry (only when menu open)
    //   Down arrow / D-pad down     — next entry
    //   Enter / Gamepad A           — confirm (arm tracking target)
    //   Esc                         — close menu
    //   F9                          — cancel an armed target
    static void ToggleMenu();
    static void MenuNext();
    static void MenuPrev();
    static void MenuConfirm();
    static void MenuClose();
    static void CancelArmedTarget();

    // Polled state so input bindings can ignore the navigation keys when
    // the menu isn't open (avoids no-op spam and keeps semantics clean).
    static bool IsMenuOpen();

    // True if a tracking target is currently armed (used by phase 3 tick).
    static bool HasArmedTarget();
    static POI  GetArmedTarget();

    // Phase 3: called every frame by Hotkeys::Tick. Internally rate-limits
    // to one announcement every few seconds while a target is armed and
    // clears the target on arrival.
    static void Tick();

    // Returns true if any of Palworld's in-game / pause menus is currently
    // active. We use this to suppress target-lock (LB) so the game's native
    // tab navigation isn't fighting our hotkey.
    static bool IsInGameMenuOpen();

    // Phase 4: target-lock mode. Hold LB → enter target mode, D-pad cycles
    // nearby enemies/Pals/NPCs, release LB commits the selection as the
    // nav-guidance target (reusing phase 3's continuous direction updates).
    static void EnterTargetMode();
    static void ExitTargetMode();
    static void TargetNext();        // d-pad down: next within category
    static void TargetPrev();        // d-pad up:   previous within category
    static void TargetCategoryNext();// d-pad right: switch to next category
    static void TargetCategoryPrev();// d-pad left:  switch to previous category
    static bool IsTargetModeActive();
};

} // namespace PalAccess
