// PalAccessibility — UE4SS C++ mod adding NVDA / Tolk screen-reader support to
// Palworld. Inherits from CppUserModBase; UE4SS finds the start_mod/uninstall_mod
// exports below.

#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/Output.hpp>
#include <DynamicOutput/DynamicOutput.hpp>

#include "TolkBridge.hpp"
#include "Speech.hpp"
#include "Hooks.hpp"
#include "Hotkeys.hpp"

using namespace RC;

class PalAccessibilityMod : public RC::CppUserModBase {
public:
    PalAccessibilityMod() : CppUserModBase() {
        ModName        = STR("PalAccessibility");
        ModVersion     = STR("0.1.0");
        ModAuthors     = STR("nordanc");
        ModDescription = STR("Screen-reader (NVDA / Tolk) and accessibility support for Palworld.");
        Output::send<LogLevel::Default>(STR("[PalAccess] constructed\n"));
    }

    ~PalAccessibilityMod() override {
        PalAccess::TolkBridge::Get().Unload();
        Output::send<LogLevel::Default>(STR("[PalAccess] destructed\n"));
    }

    // Earliest safe place to use the `Unreal` namespace. Hook installation goes here.
    auto on_unreal_init() -> void override {
        if (!PalAccess::TolkBridge::Get().Load()) {
            Output::send<LogLevel::Error>(
                STR("[PalAccess] Failed to load Tolk.dll. Make sure Tolk.dll, ")
                STR("nvdaControllerClient64.dll, and SAAPI64.dll are next to ")
                STR("Palworld-Win64-Shipping.exe.\n"));
        } else {
            auto reader = PalAccess::TolkBridge::Get().DetectScreenReader();
            Output::send<LogLevel::Default>(
                STR("[PalAccess] Tolk loaded. Screen reader: {}\n"),
                reader ? reader : L"<none / SAPI fallback>");
            PalAccess::Speech::Get().Announce(L"Palworld accessibility loaded.");
        }
        PalAccess::Hooks::Install();
    }

    auto on_program_start() -> void override {
        Output::send<LogLevel::Default>(STR("[PalAccess] program start\n"));
    }

    auto on_update() -> void override {
        PalAccess::Hotkeys::Tick();
        PalAccess::Hooks::Tick();
        PalAccess::Speech::Get().Tick();
    }
};

#define PALACCESS_EXPORT __declspec(dllexport)
extern "C" {
    PALACCESS_EXPORT RC::CppUserModBase* start_mod()                       { return new PalAccessibilityMod(); }
    PALACCESS_EXPORT void                uninstall_mod(RC::CppUserModBase* m) { delete m; }
}
