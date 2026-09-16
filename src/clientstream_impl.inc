#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "clientstream.h"
#include "platform.h"

/*
 * Implementation body lives in clientstream.c in the historical source.
 * This file is intentionally generated from that implementation by the PS2
 * integration branch so clientstream.c can wrap the three receive-buffer
 * functions on PS2 without duplicating the rest of the socket/platform code.
 *
 * NOTE: placeholder; this file is replaced by the exact previous
 * src/clientstream.c blob in the same Git tree commit.
 */
