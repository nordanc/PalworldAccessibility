# Setup guide for players

This guide installs the Palworld accessibility mod without writing or
compiling any code. It is aimed at blind and low-vision players using
NVDA. If you are a developer who wants to build the mod from source,
read `BUILD.md` instead.

The whole setup is six steps and takes about ten minutes the first time.
After that the mod loads automatically every time you start Palworld.

## Note on alpha 1

Alpha 1 of this mod is currently shipped as source code only. A prebuilt
`main.dll` is not yet attached to the GitHub release. Until alpha 2 lands,
the steps below assume you either:

1. Wait for alpha 2, which will include a ready-to-use binary, or
2. Ask a sighted friend or developer to build it for you using `BUILD.md`
   and send you the resulting `main.dll`.

Everything else in this guide — installing NVDA, installing UE4SS,
copying files into place, enabling the mod — is the same regardless of
whether you compiled the DLL yourself or someone handed it to you.

## What this mod does

It makes Palworld talk through your screen reader. Menus, settings, the
construction menu, dialogs, system notifications such as level-up and
quest, and stat readouts all become spoken. It also adds keys for
checking your health, hunger, stamina, and level without opening a menu,
plus a navigation system that points you toward nearby places of
interest using an audio beacon that pans left or right and changes
pitch when you need to climb.

For the full list of features and keys, see the README at the root of
the repository.

## What you need

You need four things installed before you start:

- **Palworld** on Steam, on the regular install path. Anywhere is fine,
  but you will need the path later.
- **NVDA**, the free screen reader from NV Access. JAWS, ZoomText,
  Window-Eyes, SuperNova, and System Access also work. If none of those
  is running, the mod falls back to Windows SAPI.
- **A controller**, optional. Keyboard works for everything. A DualSense
  or Xbox controller adds target-lock by hold of the left bumper.
- **Steam Input enabled for Palworld** if you want to use a DualSense
  controller. Without Steam Input, DualSense does not appear as XInput
  to the mod.

## Step 1: Install NVDA

If you already use NVDA, skip this step. Otherwise download it from
`https://www.nvaccess.org/download/` and run the installer. The default
options are fine. Start NVDA before launching Palworld.

## Step 2: Install UE4SS

UE4SS is the mod loader that runs this accessibility mod. It is a
separate project from this one.

Open `https://github.com/UE4SS-RE/RE-UE4SS/releases/latest` in your
browser and download the file named `UE4SS_v3.0.1.zip`, or whatever the
latest version is at the top of the page.

Extract the contents of that zip into your Palworld binaries folder. On
a standard Steam install on the C drive that folder is:

`C:\Program Files (x86)\Steam\steamapps\common\Palworld\Pal\Binaries\Win64\`

When you are done, that folder should contain a new file called
`dwmapi.dll` and a new folder called `ue4ss`, alongside the existing
`Palworld-Win64-Shipping.exe`.

Launch Palworld once after this. You will not hear anything new yet —
the mod itself is not installed. But UE4SS writes a file called
`UE4SS.log` into that same folder, which proves the loader is working.
Close the game once you see that file appear.

## Step 3: Install the accessibility mod

Go to the releases page at
`https://github.com/nordanc/PalworldAccessibility/releases/latest` and
download the release asset for the current alpha. When alpha 2 is out
this will be a zip called `PalAccessibility-alpha-2.zip` or similar.

Inside the zip you will find a folder called `PalAccessibility` and
three Tolk runtime DLLs.

Copy the three runtime DLLs — `Tolk.dll`, `nvdaControllerClient64.dll`,
and `SAAPI64.dll` — into the same Win64 folder as `dwmapi.dll`. They
have to live next to `Palworld-Win64-Shipping.exe` so Windows can find
them when the mod loads.

Copy the `PalAccessibility` folder into the `Mods` folder that UE4SS
created. The full path of the destination should be:

`C:\Program Files (x86)\Steam\steamapps\common\Palworld\Pal\Binaries\Win64\Mods\PalAccessibility\`

That folder should contain at least a `dlls` subfolder with `main.dll`
inside it, an `enabled.txt` marker file, and a `Scripts` subfolder.

## Step 4: Enable the mod

UE4SS will not load a mod until you list it in its mods file.

In your Win64 Mods folder there is a text file called `mods.txt`. Open
it in Notepad or any text editor. Above the line that says `; END`, add
this line:

`PalAccessibility : 1`

Save and close.

## Step 5: Launch the game

Start NVDA first, then launch Palworld from Steam.

Within a few seconds of the main menu appearing you should hear
"Palworld accessibility loaded." spoken through NVDA. From that moment
on, navigating menus, settings, and the construction menu will all
speak, and the stat hotkeys and navigation features are active.

## Step 6: Try the hotkeys

A short shake-down to confirm everything works:

- Press F1. You should hear your current HP.
- Press F4. You should hear your level.
- Press F5 in the open world. You should hear nearby points of interest
  by name and distance.
- Press F6. You should hear a navigation menu you can browse with the
  arrow keys and confirm with Enter.

If those four work, the mod is fully installed.

## Troubleshooting

The most common problems and what to do about them.

**You start the game and hear nothing.** Check that NVDA is running
before Palworld starts. Then check the file `UE4SS.log` in the Win64
folder. If it does not exist at all, UE4SS itself is not loading —
`dwmapi.dll` is probably not next to `Palworld-Win64-Shipping.exe`. If
the log exists but does not mention `PalAccess`, the entry in
`mods.txt` is missing or misspelled. The folder name and the name in
`mods.txt` must match exactly, including capitalisation.

**It says it loaded but no menu speaks.** This usually means the Tolk
runtime DLLs are missing from the Win64 folder. Re-copy `Tolk.dll`,
`nvdaControllerClient64.dll`, and `SAAPI64.dll` next to
`Palworld-Win64-Shipping.exe`.

**It says it loaded and speaks, but says "SAPI" instead of NVDA.** NVDA
was not running when Palworld started. Quit Palworld, start NVDA, then
launch Palworld again. Tolk only detects the screen reader at startup.

**The hotkeys stop responding after a while.** F11 prints a controller
diagnostic to UE4SS.log and re-arms the hotkeys. This is usually only
needed after a long session with several map transitions.

**My DualSense does not register for target-lock.** DualSense over
Bluetooth or USB only exposes itself as XInput when Steam Input is
enabled. In Steam, right-click Palworld, choose Properties, Controller,
and set "Override for Palworld" to "Enable Steam Input."

## Updating the mod

When a new alpha is released, you only need to repeat step 3 and
overwrite the `main.dll` and the Tolk runtime DLLs. UE4SS itself does
not need to be reinstalled, and `mods.txt` does not need to be edited
again. Settings inside Palworld are unaffected.

## Removing the mod

Delete the `PalAccessibility` folder from `Mods\`, delete the
`PalAccessibility : 1` line from `mods.txt`, and optionally delete
the three Tolk runtime DLLs from the Win64 folder. The game runs
normally again with no accessibility output.

If you want to keep UE4SS but disable only this mod, change the line
in `mods.txt` from `: 1` to `: 0` and the mod will be skipped at
startup.

## Where to report problems

Open an issue at
`https://github.com/nordanc/PalworldAccessibility/issues` with a copy
of the last twenty or so lines of `UE4SS.log` and a description of
which menu or action does not speak. Logs help diagnose mod loading
problems much faster than screenshots.
