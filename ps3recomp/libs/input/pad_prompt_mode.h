#ifndef PAD_PROMPT_MODE_H
#define PAD_PROMPT_MODE_H
/* Result: 0 original PlayStation, 1 keyboard/mouse, 2 Xbox.
 * Settings: 0 controller auto, 1 input auto, 2 PC, 3 PS, 4 Xbox. */
static inline int pad_prompt_mode(int setting, int keyboard, int xbox)
{
    if (setting == 3) return 0;
    if (setting == 4) return 2;
    if (setting == 2 || (setting != 0 && keyboard != 0)) return 1;
    return xbox ? 2 : 0;
}
#endif
