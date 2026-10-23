#pragma once
/* inFamous in-game trainer (F10).  Always built; TRAINER=0 in the environment turns it off.
 *
 * Content and cheats live here (trainer.cpp); the page itself is drawn by the port overlay
 * (ps3recomp/libs/video/rsx_ui_overlay.c) through the table in rsx_ui_trainer.h.  Cheats are applied once per frame
 * on the game's main PPU thread (cellPadGetData port 0).
 *
 * Working now:
 *   God Mode, Infinite Energy, Refill Health, Max Health x2/x5/x10, Add 1000 XP, Karma +/-100,
 *   Kill Enemies, Kill Civilians, Super Speed (x1.5 .. x5), Player Model (play as a story character).
 * Listed but locked until their memory is mapped:
 *   Super Jump, Teleport to Waypoint. */

#ifdef __cplusplus
extern "C" {
#endif

void infamous_trainer_install(void);   /* stubs.cpp ps3_load_prx_modules() */

#ifdef __cplusplus
}
#endif
