#ifndef PS3RECOMP_AUDIO_VOLUME_H
#define PS3RECOMP_AUDIO_VOLUME_H

/* Commands cross from the window thread to the audio thread; only the game's
 * Windows audio session is changed. No system/master-device volume changes. */
void ps3_audio_volume_step(int delta_percent);
/* Last applied percentage, -1 before a shortcut, -2 if output is unavailable.
 * elapsed_ms is time since that result, using the wrapping Windows tick clock. */
int ps3_audio_volume_status(unsigned* elapsed_ms);

#endif
