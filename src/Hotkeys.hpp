#pragma once

namespace RC::Unreal { class UObject; }

namespace PalAccess {

// On-demand stat readout via hardware-edge-detected hotkeys. Polled from
// CppUserModBase::on_update(). Default bindings:
//   F1  - HP
//   F2  - Hunger
//   F3  - Stamina / Sanity
//   F4  - Level (and XP if available)
//   F12 - Diagnose: dump the cached player pawn's UProperty list to
//         UE4SS.log so missing stat fields can be wired explicitly.
class Hotkeys {
public:
    // Call once per frame.
    static void Tick();

    // Called by the BeginPlay hook so we can cache the player pawn the
    // moment it spawns. Class match is loose so we catch both
    // BP_PalPlayerCharacter_* and variants.
    static void NoticePotentialPlayer(RC::Unreal::UObject* actor);

    // Called by the widget-construct hook. Lets us cache pointers to
    // in-game HUD / pause-menu gauge widgets (HP / Hunger / Stamina) so the
    // stat hotkeys can read max values that Palworld only materializes for
    // display (and never stores in SaveParameter).
    static void NoticePotentialGaugeWidget(RC::Unreal::UObject* widget);

    // Returns the validated player pawn (or nullptr if not loaded). Other
    // modules (Nav, future features) reuse this cache instead of rescanning
    // UObjects every press.
    static RC::Unreal::UObject* GetCachedPlayer();
};

} // namespace PalAccess
