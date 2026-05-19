# Build & Install Guide

This walks through every step from a clean machine to a running, NVDA-speaking
Palworld. Steps marked **(one-time)** only need to happen the first time.

## 0. Prerequisites — one-time

1. **A screen reader.** [NVDA](https://www.nvaccess.org/download/) is the
   primary target. Tolk auto-detects JAWS, Window-Eyes, ZoomText, SuperNova,
   and System Access too. SAPI is a fallback so the mod still talks if no
   reader is active.
2. **Visual Studio 2022 or 2026 Community** with the "Desktop development
   with C++" workload. The bundled CMake (3.22+) and MSVC 14.4+ toolset are
   what the project links against. `cl.exe` is **not on PATH by default** —
   either launch a "Developer Command Prompt for VS" or run
   `VC\Auxiliary\Build\vcvarsall.bat x64` before invoking cmake.
   <https://visualstudio.microsoft.com/downloads/>
3. **Rust toolchain** — UE4SS includes `patternsleuth` written in Rust.
   Install via [rustup](https://rustup.rs/) with the default stable MSVC
   toolchain (`rustup-init.exe -y --default-toolchain stable`).
4. **Git** — <https://git-scm.com/download/win>.
5. **Palworld** (Steam) — the only place we deploy the mod DLL.

## 1. Install UE4SS into Palworld — one-time

1. Download `UE4SS_v3.0.1.zip` from
   <https://github.com/UE4SS-RE/RE-UE4SS/releases/latest>.
2. Extract its **contents** (the `ue4ss/` folder and `dwmapi.dll`) into:
   ```
   C:\Program Files (x86)\Steam\steamapps\common\Palworld\Pal\Binaries\Win64\
   ```
   so that `dwmapi.dll` ends up next to `Palworld-Win64-Shipping.exe`.
3. Launch Palworld once. UE4SS writes `UE4SS.log` to that same folder; verify
   it exists and contains startup lines mentioning "UE4SS" — that confirms
   injection is working.

## 2. Drop Tolk runtime into the game folder — one-time

From this repo:

```
scripts\install-runtime.bat "C:\Program Files (x86)\Steam\steamapps\common\Palworld"
```

This copies `Tolk.dll`, `nvdaControllerClient64.dll`, and `SAAPI64.dll` next
to `Palworld-Win64-Shipping.exe`, and creates the
`Mods\PalAccessibility\{dlls,Scripts}` folders with the marker files. Tolk's
NVDA driver loads `nvdaControllerClient64.dll` from the same directory at
runtime, so they must be colocated.

## 3. Fetch UE4SS source — one-time per checkout

The build links against the `ue4ss` CMake target:

```
scripts\fetch-ue4ss.bat
```

This clones `https://github.com/UE4SS-RE/RE-UE4SS` (with submodules) into
`third_party/RE-UE4SS/`. First clone is slow because UE4SS pulls a lot of
submodules.

### 3a. UEPseudo access — REQUIRED for source build

`RE-UE4SS` has a submodule at `deps/first/Unreal` that points to
`Re-UE4SS/UEPseudo`, which mirrors private Unreal Engine headers. The repo
is **only visible to GitHub accounts linked to Epic Games' Unreal Engine
source program** — a clean clone without that link fails with
`Repository not found`. (Confirmed against this checkout: even the
SSH-rewritten HTTPS form 404s.)

To unblock the build:

1. Create an Epic Games account at <https://www.epicgames.com/>.
2. Visit <https://www.unrealengine.com/en-US/ue-on-github> and follow the
   "Linked Account" flow — sign in with the GitHub account you'll be using
   for this project.
3. Accept the Unreal Engine EULA invitation that arrives in your GitHub
   notifications. Your GitHub account is now a member of the
   `EpicGames` org and can pull `Re-UE4SS/UEPseudo`.
4. Re-run submodule init in this repo:
   ```
   cd third_party\RE-UE4SS
   git submodule update --init --recursive
   ```

If the submodule URL still resolves to `git@github.com:...` and your shell
isn't authenticated to GitHub over SSH, use HTTPS rewriting for that
command only (does not persist):
```
git -c url."https://github.com/".insteadOf=git@github.com: submodule update --init --recursive
```

## 4. Configure and build

From the repo root, in a "Developer Command Prompt for VS 2022":

```
cmake -B build -G "Visual Studio 17 2022" -A x64 ^
      -DPALWORLD_DIR="C:\Program Files (x86)\Steam\steamapps\common\Palworld"

cmake --build build --config Game__Shipping__Win64
```

The `-DPALWORLD_DIR=...` is optional — when set, the build's post-step copies
`PalAccess.dll` → `main.dll` directly into
`Pal\Binaries\Win64\Mods\PalAccessibility\dlls\`. Without it you must copy the
DLL manually.

If you prefer Ninja:

```
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Game__Shipping__Win64 ^
      -DPALWORLD_DIR="C:\Program Files (x86)\Steam\steamapps\common\Palworld"
cmake --build build
```

## 5. Enable the mod

Open (or create) `Pal\Binaries\Win64\Mods\mods.txt` and add:

```
PalAccessibility : 1
```

before the `; END` line.

## 6. Run

Start Palworld with NVDA already running. Expected behavior:

- NVDA (or SAPI) says **"Palworld accessibility loaded."** within a few seconds
  of the main menu appearing.
- `Pal\Binaries\Win64\UE4SS.log` contains `[PalAccess] Tolk loaded. Screen reader: NVDA`
  (or whatever reader is active).
- Subsequent `[PalAccess] BeginPlay:` lines list every actor class that
  spawns. Grep these for promising candidates (dialog widgets, menu panels,
  Pal characters) and feed them into `src/Hooks.cpp`'s class-name filters.

## Troubleshooting

| Symptom | Cause / fix |
| --- | --- |
| No speech, no log entries from PalAccess | Mod folder name in `mods.txt` doesn't match the folder under `Mods\`. They must both be `PalAccessibility`. |
| `UE4SS.log` says `Failed to load Tolk.dll` | The three Tolk DLLs aren't next to `Palworld-Win64-Shipping.exe`. Re-run `scripts\install-runtime.bat`. |
| Speech works but says "<none / SAPI fallback>" instead of NVDA | NVDA isn't running, or `nvdaControllerClient64.dll` is missing/wrong arch. |
| Build error `RE-UE4SS not found` | You skipped `scripts\fetch-ue4ss.bat`. |
| Build error `cannot open file 'ue4ss.lib'` | UE4SS sub-build failed earlier. Open `third_party\RE-UE4SS` and follow its own README to get it building standalone first. |
| Game crashes on launch after mod install | Almost always an arch mismatch. Confirm everything is x64 and you built with `-A x64`. |
