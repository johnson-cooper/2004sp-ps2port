#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "packet.h"
#include "platform.h"
#include "sound/wave.h"

/* wave.c owns this global; wave.h intentionally does not export it. */
extern WaveData _Wave;

static unsigned int rng_state = 0x4d595df4u;

double jrand(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return (double)(rng_state & 0x00ffffffu) / 16777215.0;
}

Packet *packet_new(int8_t *src, int length)
{
    Packet *packet = calloc(1, sizeof(Packet));
    if (!packet) {
        return NULL;
    }
    packet->data = src;
    packet->length = length;
    return packet;
}

void packet_free(Packet *packet)
{
    if (!packet) {
        return;
    }
    free(packet->data);
    free(packet);
}

void p4(Packet *packet, int value)
{
    packet->data[packet->pos++] = (int8_t)(value >> 24);
    packet->data[packet->pos++] = (int8_t)(value >> 16);
    packet->data[packet->pos++] = (int8_t)(value >> 8);
    packet->data[packet->pos++] = (int8_t)value;
}

void ip4(Packet *packet, int value)
{
    packet->data[packet->pos++] = (int8_t)value;
    packet->data[packet->pos++] = (int8_t)(value >> 8);
    packet->data[packet->pos++] = (int8_t)(value >> 16);
    packet->data[packet->pos++] = (int8_t)(value >> 24);
}

void ip2(Packet *packet, int value)
{
    packet->data[packet->pos++] = (int8_t)value;
    packet->data[packet->pos++] = (int8_t)(value >> 8);
}

int g1(Packet *packet)
{
    if (packet->pos >= packet->length) {
        return 0;
    }
    return packet->data[packet->pos++] & 0xff;
}

int g2(Packet *packet)
{
    if (packet->pos + 1 >= packet->length) {
        packet->pos = packet->length;
        return 0;
    }
    uint8_t *p = (uint8_t *)packet->data + packet->pos;
    packet->pos += 2;
    return (p[0] << 8) | p[1];
}

int g4(Packet *packet)
{
    if (packet->pos + 3 >= packet->length) {
        packet->pos = packet->length;
        return 0;
    }
    uint8_t *p = (uint8_t *)packet->data + packet->pos;
    packet->pos += 4;
    return (int)((uint32_t)p[0] << 24 |
                 (uint32_t)p[1] << 16 |
                 (uint32_t)p[2] << 8 |
                 (uint32_t)p[3]);
}

int gsmart(Packet *packet)
{
    if (packet->pos >= packet->length) {
        return 0;
    }
    int peek = packet->data[packet->pos] & 0xff;
    return peek < 128 ? g1(packet) - 64 : g2(packet) - 0xc000;
}

int gsmarts(Packet *packet)
{
    if (packet->pos >= packet->length) {
        return 0;
    }
    int peek = packet->data[packet->pos] & 0xff;
    if (peek < 128) {
        return g1(packet);
    }
    if (packet->pos + 1 >= packet->length) {
        packet->pos = packet->length;
        return 0;
    }
    return g2(packet) - 0x8000;
}

static unsigned char *read_file(const char *path, int *size_out)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size <= 0 || size > 1024 * 1024) {
        fclose(file);
        return NULL;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    unsigned char *data = malloc((size_t)size);
    if (!data) {
        fclose(file);
        return NULL;
    }

    if (fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size_out = (int)size;
    return data;
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: %s input.synth output.wav [loops]\n", argv[0]);
        return 2;
    }

    int loop_count = argc == 4 ? atoi(argv[3]) : 1;
    if (loop_count < 0) {
        loop_count = 0;
    }
    if (loop_count > 255) {
        loop_count = 255;
    }

    int synth_size = 0;
    unsigned char *synth_data = read_file(argv[1], &synth_size);
    if (!synth_data) {
        fprintf(stderr, "failed to read %s\n", argv[1]);
        return 1;
    }

    Packet input = {0};
    input.data = (int8_t *)synth_data;
    input.length = synth_size;

    Wave wave = {0};
    wave_read(&wave, &input);
    int trim = wave_trim(&wave);

    _Wave.waveBytes = calloc(441000, sizeof(int8_t));
    if (!_Wave.waveBytes) {
        fprintf(stderr, "failed to allocate wave buffer\n");
        free(synth_data);
        return 1;
    }
    _Wave.waveBuffer = packet_new(_Wave.waveBytes, 441000);
    if (!_Wave.waveBuffer) {
        fprintf(stderr, "failed to allocate packet\n");
        free(_Wave.waveBytes);
        free(synth_data);
        return 1;
    }

    tone_init_global();

    Packet *wav = wave_get_wave(&wave, loop_count);
    if (!wav || wav->pos <= 44) {
        /*
         * This is valid for some rev254 loop-only effects when loopCount=0:
         * the original generator removes the loop span and nothing remains.
         * Use a distinct exit code so the DAT builder can encode intentional
         * silence instead of treating it as corrupt Content.
         */
        fprintf(stderr, "synth produced intentional silence\n");
        free(synth_data);
        return 3;
    }

    FILE *out = fopen(argv[2], "wb");
    if (!out) {
        fprintf(stderr, "failed to open %s\n", argv[2]);
        free(synth_data);
        return 1;
    }
    if (fwrite(wav->data, 1, (size_t)wav->pos, out) != (size_t)wav->pos) {
        fprintf(stderr, "failed to write %s\n", argv[2]);
        fclose(out);
        free(synth_data);
        return 1;
    }
    fclose(out);

    printf("Synth:   %s\n", argv[1]);
    printf("WAV:     %s\n", argv[2]);
    printf("Bytes:   %d\n", wav->pos);
    printf("Trim:    %d x 20ms\n", trim);
    printf("Loops:   %d\n", loop_count);

    free(synth_data);
    return 0;
}
