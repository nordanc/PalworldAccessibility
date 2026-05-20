#pragma once

#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/FFrame.hpp>
#include <Unreal/Hooks.hpp>

namespace PalAccess {

// Convenience macro mirroring DekitaMod/UML — keeps the verbose UE4SS hook
// signature out of every call site.
#define PALACCESS_UFUNC_HOOK(F) \
    void F(RC::Unreal::UObject* Context, RC::Unreal::FFrame& Stack, void* RESULT_DECL)

class Hooks {
public:
    // Wire up every UE4SS callback we care about. Safe to call only after
    // CppUserModBase::on_unreal_init() has fired (`Unreal::` namespace is live).
    static void Install();

    // Concatenate every visible text-bearing label inside a widget, walking
    // its UProperty tree (TextBlock children, FText fields, recursively).
    // Exposed so Hotkeys can read the HUD HP/Hunger/Stamina gauge widgets.
    static std::wstring ExtractAllText(RC::Unreal::UObject* widget);

    // Per-frame poll used by deferred discovery — currently polls the
    // patch-notes WebBrowser's GetUrl() until it returns non-empty so we
    // can log the URL Palworld serves the news from.
    static void Tick();

private:
    // Fires post-BeginPlay for every AActor in the world. We use it to discover
    // Pal/NPC/widget actors and to register per-actor hooks (focus events,
    // pickups, etc.) once we know their class shape.
    static void OnActorBeginPlay(RC::Unreal::AActor* Context);

    // Fires after every blueprint script function call. We filter by class
    // and function name to capture dialog text, menu changes, item pickups, etc.
    static PALACCESS_UFUNC_HOOK(OnPostScriptFunction);

    // Dispatchers — each handles a specific in-game event family. Add more here
    // as Palworld classes/functions are discovered.
    static void TryAnnounceDialog(RC::Unreal::UObject* Context, RC::Unreal::FFrame& Stack);
    static void TryAnnounceMenuFocus(RC::Unreal::UObject* Context, RC::Unreal::FFrame& Stack);
    static void TryAnnounceItemPickup(RC::Unreal::UObject* Context, RC::Unreal::FFrame& Stack);
    static void TryAnnouncePalEvent(RC::Unreal::UObject* Context, RC::Unreal::FFrame& Stack);
};

} // namespace PalAccess
