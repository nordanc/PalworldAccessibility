# PalworldAccessibility

A UE4SS C++ mod that makes Palworld playable for blind / low-vision players.
Routes in-game text to NVDA (and other screen readers, with SAPI fallback) via
[Tolk](https://github.com/dkager/tolk), and layers on accessibility-specific
features: stat hotkeys, point-of-interest navigation with audio beacon, target
lock, and screen-reader-friendly handling of Palworld's native menus.

> **Status: alpha 2 (v0.2.0-alpha.2).** First release with a prebuilt
> binary — drag-and-drop install via the zip on the
> [latest release](https://github.com/nordanc/PalworldAccessibility/releases/latest)
> + `SETUP.md`. Day-to-day play is reachable through the screen reader. Expect
> rough edges on game UIs that haven't been mapped yet — bug reports / log
> snippets very welcome.

## Features

### Screen-reader output
- **Tolk + NVDA / JAWS / Window-Eyes / SAPI fallback.** Auto-detects the
  active reader; falls back to SAPI so the mod still speaks without a
  dedicated screen reader running.
- **Speak / Announce / FocusUpdate / Queue** layers with deduplication tuned
  per use-case (anti-chatter for repeated text, snappy for focus changes,
  sequential queue for material-cost lists).

### Native menu accessibility
- **Title menu** — every menu button (Start Game, Multiplayer, Settings,
  Credits, Exit, …) reads its real label via designer-given instance names.
- **Settings menu** — Graphics / Audio / Game / Controls / Key bindings /
  Other categories announce on selection. Inside each, every option row
  reads its label + current value (e.g. *"Mouse Sensitivity, 1"*,
  *"VSync, off"*) using UE reflection on the row's `BP_PalTextBlock_Name`
  and the per-row Switch / Slider / LR-cycler state.
- **World settings (single-player + multiplayer create world)** — same
  treatment as the in-game settings rows. Stray "caution" placeholder text
  filtered out.
- **In-game pause menu tabs** (Construction / Inventory / Map / Tech / Pal).
- **Construction menu** — categories (Production / Foundation / …) announce
  on tab change. Item focus reads the build object's **name and
  description** from `WBP_IngameMenu_Construction_Info_C` (with rich-text
  `<mapObjectName>` markup stripped). Material costs queue separately as
  *"3 of 10 Wood"* per row.
- **Inventory tabs + item slot buttons** read their semantic instance names.
- **Dialog / pop-up windows** read their main text.
- **System notifications** (level-up, quest add, achievement, autosave,
  tutorial, …) read when their text is set, with placeholder filtering and
  1-second dedupe so designer-time template strings (`（仮）`, `あいうえお`,
  `Lorem ipsum`) don't leak through.

### Stat hotkeys (anywhere in-game)
- **F1** — HP
- **F2** — Hunger (current / max from `SaveParameter.FullStomach`)
- **F3** — Stamina (SP / MP)
- **F4** — Level
- **F11** — Controller-connection sanity check (XInput)
- **F12** — Diagnose: dumps the cached player pawn + state + nested
  stat-component properties to `UE4SS.log` so unknown fields can be wired

Reader unwraps Palworld's struct-wrapped `FFixedPoint64` stats (divides by
the 1000 scale) and reads `Byte` / `Int16` / `UInt16` properties — so Level
reads as 1, not the underlying byte.

### Point-of-interest navigation
- **F5** — speak the 5 closest POIs with distance
- **F6** — open the nav menu; **↑/↓** or **D-pad up/down** to cycle,
  **Enter** or **Gamepad A** to confirm, **Esc** or **Gamepad B** to close
- **F9** — cancel an armed tracking target

Once a target is armed, an **audio beacon** (`navbeakon.wav` at the game
root) pings continuously, panned and pitched in 3D:
- **Stereo pan** based on the player camera yaw vs. target bearing — hard
  left at 90° left, hard right at 90° right, behind drops to ~55% volume
- **Pitch** rises ~1 octave per 24 m the target is above you (and drops
  for below) — climbing a tower gets a higher beacon
- **Ping rate** shrinks with distance (≈150 ms at 0 m, ~800 ms at 50 m,
  ~2000 ms at 200 m+)
- TTS announces the direction phrase ("to your right", "behind you") only
  when it changes — the pan handles the continuous "where" feedback
- **Arrival** at 2 m (3D distance) auto-clears the target

POI classifier recognises **dungeons**, **fast travel statues**,
**syndicate towers**, **arenas**, **fishing spots**, **trees / resources**,
**respawn points**.

### Target lock (gamepad-first, with keyboard mirror)
- **Hold LB** (gamepad Left Shoulder) or **Tab** to scan within 50 m for
  nearby entities, announce nearest target
- **D-pad up/down** or **↑/↓** cycle items within the current category
- **D-pad left/right** or **←/→** cycle categories
  (Enemies → Pals → Trees → Resources → Fishing → POIs, empty categories
  skipped)
- **Release LB / Tab** arms the selection; phase-3 audio beacon guidance
  takes over so you can face / approach it
- Target lock is suppressed while a native Palworld menu is open
- All hotkey/controller input is gated to Palworld's foreground window —
  no alt-tab spam

## Layout

```
PalworldAccessibility/
├── src/                       # C++ mod source
│   ├── dllmain.cpp           # CppUserModBase entry point
│   ├── TolkBridge.{hpp,cpp}  # LoadLibrary/GetProcAddress wrapper for Tolk
│   ├── Speech.{hpp,cpp}      # Speak / Announce / FocusUpdate / Queue
│   ├── Hooks.{hpp,cpp}       # UE4SS hook dispatch — every menu handler lives here
│   ├── Hotkeys.{hpp,cpp}     # F-key + XInput polling, stat reading
│   └── Nav.{hpp,cpp}         # POI scan, target lock, audio beacon (XAudio2)
├── third_party/tolk/         # Prebuilt Tolk.dll + NVDA/SAAPI client DLLs
├── third_party/RE-UE4SS/     # Cloned by scripts/fetch-ue4ss.bat (gitignored)
├── Mods/PalAccessibility/    # enabled.txt + Scripts/main.lua marker
├── scripts/
│   ├── fetch-ue4ss.bat       # Clone RE-UE4SS
│   └── install-runtime.bat   # Copy Tolk + mod skeleton into the game dir
├── CMakeLists.txt
├── SETUP.md                  # End-user install guide (screen-reader friendly)
├── BUILD.md                  # Build-from-source walkthrough
└── ARCHITECTURE.md           # How the pieces fit together
```

## Installing as a player

If you just want to use the mod and not compile it yourself, follow
`SETUP.md`. It walks through installing UE4SS, copying the mod files
into the game folder, and enabling it — no compiler or git required.

## Quick start

```
git clone https://github.com/<you>/PalworldAccessibility
cd PalworldAccessibility
scripts\fetch-ue4ss.bat
scripts\install-runtime.bat "C:\Program Files (x86)\Steam\steamapps\common\Palworld"
cmake -B build -G "Visual Studio 18 2026" -A x64 ^
      -DPALWORLD_DIR="C:\Program Files (x86)\Steam\steamapps\common\Palworld"
cmake --build build --config Game__Shipping__Win64 --target PalAccess
```

You also need:
1. UE4SS v3.0.1 dropped into `Pal\Binaries\Win64\` of your Palworld install.
2. NVDA (or your screen reader) running, OR SAPI configured.
3. **`navbeakon.wav`** placed at the game root next to
   `Pal-Windows.pak`'s parent (i.e. the same directory as
   `Palworld-Win64-Shipping.exe`'s great-grandparent — the actual `Palworld`
   folder). Optional but the nav audio beacon won't fire without it.
4. **Epic Games linked to your GitHub** to clone the `Re-UE4SS/UEPseudo`
   submodule (UE4SS source requires Epic's gated UE headers — see
   `BUILD.md` §3a).

See `BUILD.md` for the full walkthrough.

## Default key bindings

| Key       | Action                                                 |
|-----------|--------------------------------------------------------|
| F1        | Speak current HP                                       |
| F2        | Speak current Hunger (cur / max)                       |
| F3        | Speak current Stamina (cur / max)                      |
| F4        | Speak current Level                                    |
| F5        | List 5 nearest POIs with distance                      |
| F6        | Toggle nav menu                                        |
| F9        | Cancel armed nav target                                |
| F11       | Controller XInput sanity check                         |
| F12       | Diagnose: dump player class + stat components to log   |
| ↑ / ↓     | Navigate inside the nav menu / target-lock list        |
| ← / →     | Switch category inside target-lock                     |
| Enter     | Confirm nav selection                                  |
| Esc       | Close nav menu                                         |
| Tab       | Hold = enter target-lock mode (keyboard mirror of LB)  |
| LB (gamepad) | Hold = enter target-lock mode                       |
| D-pad     | Navigate nav menu / target-lock list & categories      |
| A / B (gamepad) | Confirm / close nav menu                         |

DualSense / DualShock controllers must be exposed as XInput — enable
**Steam Input** for Palworld (Properties → Controller → "Use Steam Input").
F11 confirms whether XInput sees the controller.

## How it works

1. **UE4SS** injects via `dwmapi.dll` next to `Palworld-Win64-Shipping.exe`
   and loads every C++ mod under `Pal\Binaries\Win64\Mods\<ModName>\dlls\main.dll`.
2. **`PalAccessibilityMod::on_unreal_init()`** loads `Tolk.dll`, queries the
   active screen reader, and installs the UE4SS hooks.
3. **`Hooks::OnPostScriptFunction`** is the dispatch core. It fires after
   every blueprint script function call and runs each `Handle*` dispatcher:
   button hover, click, dialog text, system notifications, construction
   info update, construction item focus, construction material row,
   construction tab select. Each filters by class + function name to avoid
   firing on noise.
4. **Reflection over UProperty** lets us read displayed text from any
   widget without per-class hooks — we walk `ForEachPropertyInChain`,
   recurse into `FStructProperty` for stat values, follow `ObjectProperty`
   into nested `BP_PalTextBlock_C` children, and unwrap `FFixedPoint64` by
   dividing inner int64 by 1000.
5. **`Hotkeys::Tick()`** runs every frame and polls keyboard
   (`GetAsyncKeyState`) + XInput. All input is gated to Palworld's
   foreground window.
6. **`Nav::Tick()`** runs the audio guidance loop when a target is armed:
   computes 2D bearing + 3D distance, picks ping interval, computes pan
   from yaw, picks pitch from vertical offset, plays the WAV via XAudio2
   with a per-call output matrix and frequency ratio. Announces direction
   only when it changes buckets.

## Extending it

Almost every menu hook lives in `src/Hooks.cpp` as a `Handle*` function.
Pattern:

```cpp
void HandleMyEvent(Unreal::UObject* Context, Unreal::FFrame& Stack) {
    if (ClassNameOf(Context) != L"WBP_TheRightClass") return;
    if (!FunctionNameIs(Stack, L"TheFunction")) return;
    auto txt = ReadFirstFTextParam(Stack);
    if (!txt.empty()) Speech::Get().Announce(std::wstring_view(txt));
}
```

Then call it from `Hooks::OnPostScriptFunction`. New widget class names
get logged automatically — open `UE4SS.log` and search for
`[PalAccess] panel Construct:` after entering an unmapped menu.

For new hotkeys, add an entry to the `table[]` in `Hotkeys::Tick` with a
virtual key code and a lambda. XInput-driven actions go in the same
function under the gamepad polling block.

## Known gaps / roadmap

- [ ] **DualSense / DualShock without Steam Input** — currently requires
  Steam Input. A raw HID path is possible but not yet implemented.
- [ ] **Dynamic foliage trees** — only choppable resource-tree spawners
  show up in nav. Background foliage trees are UMG-instanced, not actor
  classes, so they're invisible to `ForEachUObject`.
- [ ] **Player MaxHP** — Palworld computes player-side MaxHP at runtime
  via a native getter; the SaveParameter field is 0 for player characters.
  We announce current HP only. Walking via `K2_GetActorLocation` /
  `ProcessEvent` works fine; calling the MaxHP getter is the next step
  if we want it.
- [ ] **Combat events** — damage taken, low-HP warnings, status effects
  not yet wired.
- [ ] **Inventory item-slot content** — slot focus reads "Inventory" tab
  name but not the item the cursor is on.
- [ ] **Chat / multiplayer messages** — not yet announced.
- [ ] **User-tunable config** — verbosity, mute keys, beacon volume, etc.

## License

[MIT](LICENSE). Tolk is LGPLv3 (its WAV / DLL binaries are not modified;
they're carried as runtime dependencies).

## Credits

- [Tolk](https://github.com/dkager/tolk) (LGPLv3) — Davy Kager
- [RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) — UE4SS-RE team
- C++ mod scaffold pattern from [Dekita/UE4SS-CppModBase](https://github.com/Dekita/UE4SS-CppModBase)
- Speech dedup layer adapted from prior Siralim Ultimate accessibility mod
- Palworld is © Pocketpair, Inc. — this mod modifies no game files and
  injects in-process at runtime via UE4SS.
