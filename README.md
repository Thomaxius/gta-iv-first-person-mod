# GTA IV Complete Edition - First-Person Mod

A first person mod for GTA IV. Main difference over C06alt's is that it also contains the ability to trigger first person for cutscenes, 
much like how Zolika's trainer for older versions of GTA IV did. It is currently in beta stage, so improvements might or might not be released later on.

Disclaimer: this is completely vibe-coded over brainstorming sessions with Claude.

## Installation
You need an .asi loader such as Ultimate ASI Loader. It has also been tested only with Fusion Fix, but most likely works without it, as well.
It is also used together with the RTX Remix compatibility mod (xoxor4d's gta4-rtx).

Place the .asi to `plugins` folder. The mod creates `FirstPersonCutscene.ini` next to it on first launch
(or copy the one from this repo there), see *Settings*.

## Build
x86 DLL, `/MT`, no PCH, output extension `.asi`, drop next to `GTAIV.exe`
(loads via FusionFix's Ultimate ASI Loader). Single file: `src/FirstPersonCutscene.cpp`.

From an x86 Visual Studio developer prompt:

```
cl /LD /MT /EHsc /O2 /Fe:FirstPersonCutscene.asi src\FirstPersonCutscene.cpp /link /MACHINE:X86 user32.lib
```

## Keys
Only **F7** and **F5** (both rebindable, see *Settings*) work outside debug mode. **Ctrl + F7** toggles debug mode, which unlocks every other key below.

| Key | Action |
|---|---|
| F7 | enable / disable first person |
| Ctrl + F7 | toggle debug mode (unlocks the keys below) |
| F9 | recenter the view |
| F11 / F12 | eye height − / + |
| ← / → | eye forward offset − / +. While aiming (or with Ctrl held) they tune the **current weapon's** aim eye offset instead, see *Aiming* |
| ↑ / ↓ | mouse sensitivity + / − |
| PgUp / PgDn | FOV + / − |
| 7 / 8 | cycle which cutscene actor the camera follows |
| 0 | un-pin the cutscene actor, back to automatic |
| J | force head-hide off (it's automatic in first person; hair goes with it) |
| B | head-bone rotation test; then ← / → pick the bone axis |
| K | aim alignment on / off |
| L | aim eye-shift (parallax) on / off |
| M | aim side: centered (default) / the game's right shoulder / left shoulder |
| N | experimental: also raise the aim ray to eye height |
| F5 | save your current settings into the `.ini`, with an on-screen confirmation (works outside debug mode) |
| Ctrl + F5 | reload the `.ini` (also outside debug mode) |
| F6 | native invoker on / off (on by default — debug only) |
| F8 | dump diagnostics to `FirstPersonCutscene.log` |

Eye height, eye forward and FOV are remembered **separately for on foot, in a car and on a
train / subway** — the keys tune whichever context you're currently in. Tune live in debug
mode, then press **F5** to keep the values (see *Settings*).

## Settings (.ini)
All settings live in `FirstPersonCutscene.ini`, next to the `.asi` and with the same name. This
repo has a copy with the defaults; if the file is missing the mod creates one on first launch,
with a comment above every setting. A missing or invalid line just falls back to its default,
and deleting the file resets everything.

| Section | What it controls |
|---|---|
| `[General]` | `StartEnabled`, `ToggleKey` (F1–F12, a letter / digit or a hex code), `SaveKey` (same, or `none`), `ShowMessages`, `DebugMode`, `HideHead`, `MouseSensitivity`, `InvertY`, `MaxPitchDegrees` |
| `[OnFoot]` `[Car]` `[Train]` | `EyeHeight` and `EyeForward` (metres) and `FovBoost` (degrees added to the game's own FOV) for each context |
| `[Aim]` | `AlignToAimCamera`, `EyeOnAimRay`, `AimSide` (`center` / `right` / `left`), `AimRayToEyeHeight` |
| `[AimEyeForward]` | eye offset while aiming, per weapon slot (`Slot0`–`Slot15`) or for one weapon (`Weapon<type>`) |

For a different screen size or an ultra-wide, start with `FovBoost` (per context) and
`MouseSensitivity`.

### Adjusting settings
Pick whichever suits you, they mix fine.

**Live, in game** (you see the result straight away)
1. Press **Ctrl + F7** to switch on debug mode.
2. Tune with the keys from the table above: **F11 / F12** eye height, **← / →** eye forward,
   **PgUp / PgDn** FOV, **↑ / ↓** mouse sensitivity. They apply to whatever you're in right
   now (on foot, car or train), so sit in a car to tune the car values.
3. Press **F5** to save the result into the `.ini` (see below).
4. Press **Ctrl + F7** again to leave debug mode.

**By editing the file**
Change a value in any text editor, then press **Ctrl + F5** in game to reload it, no restart
needed. Only `StartEnabled` and `DebugMode` wait for the next launch.

### Saving (dumping) your settings
**F5** writes your current settings into the `.ini` in one go, and "First person: settings saved
to the .ini" shows on screen for a few seconds. It works with or without debug mode. It saves:
- eye height, eye forward and FOV for on foot, car and train,
- mouse sensitivity, invert Y, look limit, head hiding,
- the aim options and the per-weapon aim offsets, including any you tuned with **← / →** while aiming.

It leaves `StartEnabled`, `ToggleKey`, `SaveKey` and `DebugMode` alone, and keeps your comments
and hand-edited lines. **Ctrl + F5** does the reverse: it reloads the file, so anything you tuned
live and haven't saved is replaced by what's in it.

`SaveKey` rebinds the key (`none` turns it off) and `ShowMessages = 0` hides the on-screen text.
Either way the result is written to `FirstPersonCutscene.log`, which also lists the loaded values
at startup.

## How it works
- **Camera:** hook `CopyCameraFrame` (RVA 0x83E398), overwrite the final camera
  matrix each frame. Reaches gameplay, vehicle and cutscene cameras.
- **Natives:** self-contained invoker (FusionFix method). Resolved & called from a
  hook on the CGame per-frame process chain (sim thread) — natives crash elsewhere.
  GTA IV natives return values through a trailing output-pointer arg.
- **Head bone:** `CPed::GetBoneMatrix` (RVA 0x5E70B0) for the real head-bone matrix.
- **Cutscene actor:** `GET_CUTSCENE_PED_POSITION` slot index, per-cutscene table
  (`Vla4_a` → slot 10).
- **Head hide:** `SET_DRAW_PLAYER_COMPONENT` for HEAD / TEEF / FACE (and HAIR),
  auto-applied whenever first person is active.
- **Look:** absolute world yaw, seeded once per FP session / per cutscene and moved
  only by the mouse, so strafing and shot cuts don't drag the view. In a vehicle
  the base yaw tracks the vehicle heading. `IS_PED_RAGDOLL` auto-switches to the
  head bone's own rotation while you're down, so getting hit and tumbling actually
  rolls the camera with your head.

### Aiming
The game aims from its own aim camera (FusionFix calls it `CCamAimWeapon`: an
over-the-right-shoulder camera that only exists while you hold RMB), not from the camera we
render, so shots used to miss the crosshair. On foot, while that camera is live the mod:
- takes its yaw / pitch (easing in over ~80 ms, so entering aim doesn't snap),
- rewrites its local shoulder offset every tick so the bullet ray starts over your head
  instead of the right shoulder (side selectable with **M**), and
- slides the eye onto that ray, so the crosshair is the bullet line.

The current weapon is read with `GET_CURRENT_CHAR_WEAPON` + `GET_WEAPONTYPE_SLOT`. Each weapon
slot (category) has an eye-forward offset applied while aiming, so long guns' stocks don't clip
the screen; assault rifles (M4, AK47...) default to −8 cm.

## Known issues
- Wrong actor is chosen during cutscenes sometimes. You can fix this by enabling debug mode with CTRL + F7 and then cycling peds with 7 and 8.
- Larger weapons might clip on the screen a bit when shooting in first person mode. Assault rifles are compensated by default; for other weapons, in debug mode aim with the weapon and tap ← / → until the clipping stops (the log prints the weapon, slot and value).
- Drive-bys still use the game's own aim, so it is off completely.
- The head casts no shadow, since it is hidden by not drawing it.

## Contributing 
Feel free to help me make this better in every way. Hit me up in Discord or create issues in Github.

## Credits
- C06alt for his excellent first person mod, for inspiration and pointers how to make this possible.
- Zolika for his equally excellent trainer and especially the cutscene part.
- The Fusion Fix team for their tireless work on keeping the GTA IV scene alive over the years with their great improvements.
- Claude. I wouldn't have had the patience to go get this done without AI.
