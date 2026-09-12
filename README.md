# GTA IV Complete Edition - First-Person Mod by Thomaxius

A first person mod for GTA IV. Main difference over C06alt's is that it also contains the ability to trigger first person for cutscenes, 
much like how Zolika's trainer for older versions of GTA IV did. It is currently in beta stage, so improvements might or might not be released later on.

Disclaimer: this is completely vibe-coded over brainstorming sessions with Claude.

## Installation
You need an .asi loader such as Ultimate ASI Loader. It has also been tested only with Fusion Fix, but most likely works without it, as well.

Place the .asi to `scripts` folder

## Build
x86 DLL, `/MT`, no PCH, output extension `.asi`, drop next to `GTAIV.exe`
(loads via FusionFix's Ultimate ASI Loader). Single file: `FirstPersonCutscene.cpp`.

## Keys
| Key | Action |
|---|---|
| F7 | enable / disable |
| F10 | cycle mode (3 = first person; 0–2 are old cutscene experiments) |
| F9 | recenter the view |
| F11 / F12 | eye height − / + |
| ← / → | eye forward offset − / + (in mode 3) |
| ↑ / ↓ | mouse sensitivity + / − (in mode 3) |
| PgUp / PgDn | FOV + / − |
| 7 / 8 | cycle which cutscene actor the camera follows |
| J | force head-hide off (it's automatic in first person; hair goes with it) |
| B | head-bone rotation test; then ← / → pick the bone axis |
| F6 | native invoker on / off (on by default — debug only) |
| F8 | dump diagnostics to `FirstPersonCutscene.log` |

Eye height, eye forward and FOV are remembered **separately for on-foot vs in a
vehicle** — the keys tune whichever context you're currently in.

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

## Contributing 
Feel free to help me make this better in every way. Hit me up in Discord or create issues in Github.

## Credits
- C06alt for his excellent first person mod, for inspiration and pointers how to make this possible.
- Zolika for his equally excellent trainer and especially the cutscene part.
- The Fusion Fix team for their tireless work on keeping the GTA IV scene alive over the years with their great improvements.
- Claude. I wouldn't have had the patience to go get this done without AI.