#ifndef RSX_DISPLAY_RESOLUTION_H
#define RSX_DISPLAY_RESOLUTION_H
#include <stdio.h>
/* Exact output sizes retain the game's camera aspect, with centered black bars.
 * Numeric RES_SCALE values remain supported for older settings files. */
static inline int rsx_display_resolution(const char* text, unsigned* w, unsigned* h)
{
    unsigned x, y; char tail;
    if (!text || sscanf(text, "%ux%u%c", &x, &y, &tail) != 2 ||
        x < 640 || y < 480 || x > 7680 || y > 4320) return 0;
    *w = x; *h = y; return 1;
}
static inline void rsx_display_fit(unsigned w, unsigned h, unsigned gw, unsigned gh,
                                    unsigned* cw, unsigned* ch)
{
    if ((unsigned long long)w * gh <= (unsigned long long)h * gw) {
        *cw = w & ~1u; *ch = (unsigned)((unsigned long long)*cw * gh / gw) & ~1u;
    } else {
        *ch = h & ~1u; *cw = (unsigned)((unsigned long long)*ch * gw / gh) & ~1u;
    }
}
#endif
