#pragma once
/* In-game trainer page of the port overlay (rsx_ui_overlay.c), opened with F10.
 *
 * The runtime only draws and navigates; the TITLE owns the content and the cheats.  A title installs a callback table
 * (g_uiov_trainer) and, if it needs to touch guest memory safely, a per-frame hook on the game's main PPU thread
 * (g_pad_title_tick_fn, called from cellPadGetData(port 0) inside an HLE call).
 * Titles that install nothing get no trainer: F10 does nothing.
 *
 * Threading: row()/hud()/status() are called on the RENDER thread every frame the page (or the HUD line) is shown, so
 * they must only read; change() is also called on the render thread and should just record a request that the title's
 * main-thread hook applies. */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    UIOV_TR_CHOICE = 0,   /* Left/Right cycles the value, Enter = Right            */
    UIOV_TR_ACTION = 1,   /* Enter / click runs it; value = last result (optional) */
    UIOV_TR_HEADER = 2,   /* section title, not selectable                         */
    UIOV_TR_LOCKED = 3    /* listed but not available yet; value = why             */
};

typedef struct uiov_trainer_row {
    int  kind;            /* UIOV_TR_* */
    int  on;              /* CHOICE: highlight the value as active (non-default) */
    char label[48];
    char value[64];
} uiov_trainer_row;

typedef struct uiov_trainer_api {
    const char* title;                                 /* page heading, e.g. "TRAINER"                      */
    int  (*count)(void);                               /* number of rows                                    */
    int  (*row)(int index, uiov_trainer_row* out);     /* fill one row; 0 = skip it                         */
    void (*change)(int index, int dir);                /* CHOICE: dir -1/+1;  ACTION: dir 0 (activate)      */
    int  (*status)(char* text, size_t capacity);       /* one line under the heading (player state), 0 none */
    int  (*hud)(char* text, size_t capacity);          /* short "active cheats" line drawn while the page is
                                                          closed, 0 = nothing active                        */
} uiov_trainer_api;

/* rsx_ui_overlay.c; NULL until a title installs a table. */
extern const uiov_trainer_api* volatile g_uiov_trainer;

/* cellPad.c; called once per cellPadGetData(port 0) on the game's main PPU thread, inside an HLE call
 * (g_active_ctx valid), between game frames.  NULL = nothing. */
extern void (*volatile g_pad_title_tick_fn)(void);

/* cellPad.c; port 0 as the game last received it (previous frame when read from g_pad_title_tick_fn):
 * left stick x,y (0..255, 128 = centre) and the digital buttons (DIGITAL2 << 8 | DIGITAL1). */
extern volatile unsigned char g_pad_title_lstick[2];
extern volatile unsigned g_pad_title_digital;

#ifdef __cplusplus
}
#endif
