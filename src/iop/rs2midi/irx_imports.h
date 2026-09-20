/*
 * Minimal IRX import header for the rs2midi companion module.
 *
 * PS2SDK traditionally keeps one irx_imports.h next to each IOP module's
 * imports.lst. Keep this deliberately small: rs2midi needs SIF RPC, thread
 * services, and LIBSD voice-control import macros.
 */
#ifndef RS2MIDI_IRX_IMPORTS_H
#define RS2MIDI_IRX_IMPORTS_H

#include <irx.h>
#include <loadcore.h>
#include <sifcmd.h>
#include <thbase.h>
#include <libsd.h>

#endif /* RS2MIDI_IRX_IMPORTS_H */
