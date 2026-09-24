#pragma once

#ifdef __PS2__
#include <stdbool.h>

void ps2_music_request(int id, int jingle_delay_ms);
void ps2_sfx_request(int id, int loops, int delay);
bool ps2_music_play(const char *name);
void ps2_music_stop(void);
void ps2_music_update(void);
bool ps2_music_emergency_guard_active(void);
void ps2_music_set_volume(float volume);
void ps2_music_shutdown(void);
#endif
