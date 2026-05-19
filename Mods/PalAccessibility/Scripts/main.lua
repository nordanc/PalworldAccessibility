-- PalAccessibility — Lua side, kept tiny on purpose.
--
-- The heavy lifting (Tolk loading, hook installation, speech dedup) lives in
-- the C++ DLL at Mods/PalAccessibility/dlls/main.dll. UE4SS still expects a
-- Scripts/main.lua to exist for the mod folder to be picked up; this file is
-- the marker plus a place to add user-tunable accessibility flags later
-- (e.g. announce-on-low-HP threshold, verbosity level).

print("[PalAccess] Lua loader ready (C++ mod handles all behavior)")
