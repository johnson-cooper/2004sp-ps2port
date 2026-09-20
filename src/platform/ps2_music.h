#pragma once

#ifdef __PS2__
#include <stdbool.h>

bool ps2_music_play(const char *name);
void ps2_music_stop(void);
void ps2_music_update(void);
void ps2_music_set_volume(float volume);
void ps2_music_shutdown(void);
#endif
