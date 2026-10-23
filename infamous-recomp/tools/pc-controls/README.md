# PC prompt artwork

`source.png` is the ImageGen-created keyboard/mouse sprite sheet, matched to the
original inFamous HUD's charcoal bevels and worn white lettering. It contains no
original game textures. `../pc_control_art.py` defines the reviewed crop boxes.
Packing trims transparent padding, scales uniformly and centers each sprite in
the original cell; glyph metrics, texture dimensions and UVs stay unchanged.

Build from the repository root after extracting assets from your own game:

```powershell
python infamous-recomp/tools/make_kbm_icons.py
python infamous-recomp/tools/make_kbm_prompts.py .local/usa/xpp/core.xpp infamous-recomp/assets/kbm_prompts.bin .local/pc-prompts/hud-preview.png
```

The USA asset pipeline runs both builders automatically. The HUD builder detects
a complete EU or USA texture-ID set, rather than using EU IDs on USA data.
Generated runtime binaries remain in the ignored `infamous-recomp/assets` folder.
Restart the game after rebuilding them; the runtime loads them once per session.
The icon builder also adds 19 PC glyphs to unused space in `ui_font.bin` for
system dialogs such as the opening autosave notice. The overlay chooses these
glyphs using the same prompt setting and input-device state. On a fresh launch
it defaults to PC prompts until controller input is detected. Space confirms
the notice; Enter also works. The controller glyphs remain available.

Bindings match `cellPad.c`: WASD movement, mouse look, Space/C/F/E for
Cross/Circle/Square/Triangle, right/left mouse for L1/R1, Q/R for L2/R2,
V/middle mouse for L3/R3, Esc pause, Tab map, Ctrl precision mode and arrows
for the remaining directional controls. F8 releases/captures the pointer.
Shift moves at full stick magnitude. Pause menus also accept Enter to confirm,
Backspace to go back and WASD to navigate.

`KBM_GLYPHS=1` follows the most recently used input device (default), `2` forces
PC icons and `0` keeps controller icons. Settings can override the environment.
Only matched controller prompt textures are replaced; unrelated icons and the
logo remain intact.

## Controller families

The same builders also produce `xbox_icons.bin` and `xbox_prompts.bin`, with
A/B/X/Y, LB/RB, LT/RT, LS/RS and Menu/View labels. Xbox and PC variants share
the exact original-texture signature, but use separate renderer cache keys,
so changing devices refreshes the prompt. PlayStation uses the original guest
texture bytes. System dialogs have both variants in their separate font atlas.

Automatic follows activity on player one's controller: XInput uses Xbox,
and the optional SDL3 backend keeps original PlayStation glyphs for Sony pads.
SDL3 Steam controllers use Xbox labels. Other unclassified SDL3 pads retain
the original prompts. A second controller cannot change player one's prompts.

In-game Settings > Button Prompts also offers explicit PlayStation (`3`) and
Xbox (`4`) choices. These override device detection. They are useful when a
PlayStation controller is exposed as virtual Xbox by DS4Windows or Steam Input:
XInput cannot identify the physical controller behind that virtual device.
The choices can also be set with `KBM_GLYPHS=3` or `4` (saved Settings take
precedence). Native Sony input requires the existing optional SDL3 backend;
the local 4K build bundles SDL3 3.4.18 and enables that backend. CMake copies
the matching x64 SDL3.dll beside the executable after each build. Sony pads
also work in system dialogs, including the opening autosave notice.
