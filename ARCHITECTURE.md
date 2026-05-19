# Architecture

A short tour of how the pieces fit together, intended for anyone (including
future-you) extending the mod.

## Injection path

```
Palworld-Win64-Shipping.exe
    └ loads dwmapi.dll from its own directory (Windows DLL search order)
        └ that "dwmapi.dll" is actually UE4SS, which forwards real dwmapi
          calls to the system DLL while also bootstrapping itself
            └ UE4SS scans Mods\<name>\dlls\main.dll for each enabled mod
                └ calls start_mod() which returns our PalAccessibilityMod
                  instance — UE4SS owns the lifetime
```

## Runtime stack

```
                   Palworld blueprints                NVDA / JAWS / SAPI
                          │                                  ▲
              UFunction call / Actor BeginPlay               │
                          │                          (speech / braille)
                          ▼                                  │
                   UE4SS hook system                         │
                          │                                  │
                          ▼                                  │
   PalAccess::Hooks  (Hooks.cpp)                             │
   - filters by class name + function name                   │
   - reads input params off the script stack                 │
                          │                                  │
                          ▼                                  │
   PalAccess::Speech  (Speech.cpp)                           │
   - dedup window (200 ms last-utterance match)              │
   - recent-30 buffer (anti-chatter)                         │
   - UTF-8 -> wchar_t conversion                             │
                          │                                  │
                          ▼                                  │
   PalAccess::TolkBridge (TolkBridge.cpp)                    │
   - LoadLibrary("Tolk.dll") + GetProcAddress                │
   - Tolk_Load, Tolk_Output, Tolk_Silence, ...               │
                          │                                  │
                          ▼                                  │
                       Tolk.dll  ────────────────────────────┘
   (which in turn loads nvdaControllerClient64.dll, SAAPI64.dll, etc.)
```

## Why dynamic-load Tolk instead of static link?

Tolk's header conditionally `__declspec(dllimport)`s its functions when not
building Tolk itself, so static linking would force a build-time dependency on
`Tolk.lib`. Dynamic loading via `LoadLibrary` + `GetProcAddress` keeps the
build self-contained, makes Tolk an optional runtime dependency (mod degrades
gracefully if the user forgot to copy the DLLs), and matches the pattern from
the user's Siralim accessibility mod.

## Where the engine-specific work happens

`Hooks.cpp` is where Palworld-specific knowledge accumulates. Each dispatcher
function (`TryAnnounceDialog`, `TryAnnounceMenuFocus`, etc.) is:

1. A class-name filter, e.g. `ClassNameStartsWith(Context, L"WBP_Dialog")`.
2. A function-name filter, e.g. `FunctionNameIs(Stack, L"SetText")`.
3. A param struct that mirrors the blueprint UFunction's input layout, cast
   from `Stack.Locals()` via `GetInputParameters<T>`.
4. A call to `Speech::Speak` or `Speech::Announce`.

The current dispatchers ship with placeholder names. The discovery loop in
`Hooks::OnActorBeginPlay` logs the first 50 unique actor classes to
`UE4SS.log` — grep that for `BP_Pal*`, `WBP_*`, `W_*` candidates and replace
the placeholders.

## Threading

`Speech::Get()` is a Meyers singleton with an internal `std::mutex`, so the
`Speak` / `Announce` calls are safe from any UE4SS callback. Tolk functions
themselves are documented as asynchronous and thread-safe.

## Adding non-speech features later

The mod is a normal `CppUserModBase`. To add (for example) a "press F1 to
read the current waypoint heading" hotkey, hook into `on_update()` to poll
`GetAsyncKeyState`, then walk the player controller's compass widget via
`UObjectGlobals::StaticFindObject` and feed the heading into `Speech`.
