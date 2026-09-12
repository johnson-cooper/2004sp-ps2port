#include <stdlib.h>

#include "animframe.h"
#include "ondemand.h"
#include "packet.h"
#include "platform.h"

extern AnimBaseData _AnimBase;
AnimFrameData _AnimFrame = {0};

void animframe_free_global(void) {
    for (int i = 0; i < _AnimFrame.count; i++) {
        if (_AnimFrame.instances[i]) {
            free(_AnimFrame.instances[i]->groups);
            free(_AnimFrame.instances[i]->x);
            free(_AnimFrame.instances[i]->y);
            free(_AnimFrame.instances[i]->z);
            free(_AnimFrame.instances[i]);
        }
    }
    free(_AnimFrame.instances);
}

void animframe_unpack(Jagfile *models) {
    Packet *head = jagfile_to_packet(models, "frame_head.dat");
    Packet *tran1 = jagfile_to_packet(models, "frame_tran1.dat");
    Packet *tran2 = jagfile_to_packet(models, "frame_tran2.dat");
    Packet *del = jagfile_to_packet(models, "frame_del.dat");

    const int total = g2(head);
    const int count = g2(head);
    _AnimFrame.count = count + 1;
    _AnimFrame.instances = calloc(_AnimFrame.count, sizeof(AnimFrame *));
    int *labels = calloc(500, sizeof(int));
    int *x = calloc(500, sizeof(int));
    int *y = calloc(500, sizeof(int));
    int *z = calloc(500, sizeof(int));
    for (int i = 0; i < total; i++) {
        int id = g2(head);
        AnimFrame *frame = _AnimFrame.instances[id] = calloc(1, sizeof(AnimFrame));
        frame->delay = g1(del);
        int baseId = g2(head);
        AnimBase *base = _AnimBase.instances[baseId];
        frame->base = base;
        int length = g1(head);
        int last_group = -1;
        int current = 0;
        int flags;
        for (int j = 0; j < length; j++) {
            flags = g1(tran1);
            if (flags > 0) {
                if (base->types[j] != OP_BASE) {
                    for (int group = j - 1; group > last_group; group--) {
                        if (base->types[group] == OP_BASE) {
                            labels[current] = group;
                            x[current] = 0;
                            y[current] = 0;
                            z[current] = 0;
                            current++;
                            break;
                        }
                    }
                }
                labels[current] = j;
                int default_value = 0;
                if (base->types[labels[current]] == OP_SCALE) {
                    default_value = 128;
                }
                if ((flags & 0x1) == 0) {
                    x[current] = default_value;
                } else {
                    x[current] = gsmart(tran2);
                }
                if ((flags & 0x2) == 0) {
                    y[current] = default_value;
                } else {
                    y[current] = gsmart(tran2);
                }
                if ((flags & 0x4) == 0) {
                    z[current] = default_value;
                } else {
                    z[current] = gsmart(tran2);
                }
                last_group = j;
                current++;
            }
        }
        frame->length = current;
        frame->groups = calloc(current, sizeof(int));
        frame->x = calloc(current, sizeof(int));
        frame->y = calloc(current, sizeof(int));
        frame->z = calloc(current, sizeof(int));
        for (flags = 0; flags < current; flags++) {
            frame->groups[flags] = labels[flags];
            frame->x[flags] = x[flags];
            frame->y[flags] = y[flags];
            frame->z[flags] = z[flags];
        }
    }

    free(labels);
    free(x);
    free(y);
    free(z);
    packet_free(head);
    packet_free(tran1);
    packet_free(tran2);
    packet_free(del);
}

// rev254 delivers animation frames via ondemand.zip (archive index 1, "2.<fileId>" entries) instead
// of the old monolithic frame_head.dat/frame_tran1.dat/frame_tran2.dat/frame_del.dat scheme above.
// Client3 was still unpacking the stale rev225 archive for this - applying rev225-shaped animation
// transforms to correctly-decoded rev254 model geometry is exactly what produced the stretched/
// twisted-looking limbs reported after the model on-demand fix landed. Confirmed against the
// reference client: the per-vertex-group gsmart/OP_BASE/OP_SCALE decode logic below is byte-for-byte
// identical to the block above - only the container changed. Two differences from the old format:
// each on-demand file holds a BATCH of frames (walk the head section's leading "total" count) sharing
// ONE embedded AnimBase (parsed inline after the del section, not looked up from a global id-keyed
// table - there is no per-frame baseId field anymore), and there's no single global frame-count
// header field to size _AnimFrame.instances[] up front, so a first pass finds the max frame id
// across every anim-category file before allocating it.
void animframe_unpack_ondemand(void) {
    int file_count = ondemand_file_count();

    int max_id = -1;
    for (int i = 0; i < file_count; i++) {
        int archive;
        int file_id;
        if (!ondemand_get_entry_info(i, &archive, &file_id) || archive != 1) {
            continue;
        }
        int size = 0;
        int8_t *data = ondemand_get_by_index(i, &size);
        if (!data) {
            continue;
        }
        Packet head = {0};
        head.data = data;
        int total = g2(&head);
        for (int f = 0; f < total; f++) {
            int id = g2(&head);
            if (id > max_id) {
                max_id = id;
            }
            g1(&head); // groupCount - not needed in this counting pass
        }
        free(data);
    }

    if (max_id < 0) {
        rs2_error("animframe_unpack_ondemand: no anim frames found in ondemand.zip\n");
        return;
    }

    _AnimFrame.count = max_id + 1;
    _AnimFrame.instances = calloc(_AnimFrame.count, sizeof(AnimFrame *));
    int *labels = calloc(500, sizeof(int));
    int *x = calloc(500, sizeof(int));
    int *y = calloc(500, sizeof(int));
    int *z = calloc(500, sizeof(int));

    for (int i = 0; i < file_count; i++) {
        int archive;
        int file_id;
        if (!ondemand_get_entry_info(i, &archive, &file_id) || archive != 1) {
            continue;
        }
        int size = 0;
        int8_t *data = ondemand_get_by_index(i, &size);
        if (!data || size < 8) {
            free(data);
            continue;
        }

        Packet trailer = {0};
        trailer.data = data;
        trailer.pos = size - 8;
        int head_length = g2(&trailer);
        int tran1_length = g2(&trailer);
        int tran2_length = g2(&trailer);
        int del_length = g2(&trailer);

        int pos = 0;
        int head_offset = pos;
        pos += head_length + 2;
        int tran1_offset = pos;
        pos += tran1_length;
        int tran2_offset = pos;
        pos += tran2_length;
        int del_offset = pos;
        pos += del_length;
        int base_offset = pos;

        Packet head = {0};
        head.data = data;
        head.pos = head_offset;
        Packet tran1 = {0};
        tran1.data = data;
        tran1.pos = tran1_offset;
        Packet tran2 = {0};
        tran2.data = data;
        tran2.pos = tran2_offset;
        Packet del = {0};
        del.data = data;
        del.pos = del_offset;
        Packet base_buf = {0};
        base_buf.data = data;
        base_buf.pos = base_offset;

        AnimBase *base = calloc(1, sizeof(AnimBase));
        base->length = g1(&base_buf);
        base->types = calloc(base->length, sizeof(int));
        base->labels = calloc(base->length, sizeof(int *));
        base->labels_count = calloc(base->length, sizeof(int));
        for (int j = 0; j < base->length; j++) {
            base->types[j] = g1(&base_buf);
        }
        for (int j = 0; j < base->length; j++) {
            int group_count = g1(&base_buf);
            base->labels_count[j] = group_count;
            base->labels[j] = calloc(group_count, sizeof(int));
            for (int k = 0; k < group_count; k++) {
                base->labels[j][k] = g1(&base_buf);
            }
        }

        int total = g2(&head);
        for (int fi = 0; fi < total; fi++) {
            int id = g2(&head);
            AnimFrame *frame = _AnimFrame.instances[id] = calloc(1, sizeof(AnimFrame));
            frame->delay = g1(&del);
            frame->base = base;
            int length = g1(&head);
            int last_group = -1;
            int current = 0;
            int flags;
            for (int j = 0; j < length; j++) {
                flags = g1(&tran1);
                if (flags > 0) {
                    if (base->types[j] != OP_BASE) {
                        for (int group = j - 1; group > last_group; group--) {
                            if (base->types[group] == OP_BASE) {
                                labels[current] = group;
                                x[current] = 0;
                                y[current] = 0;
                                z[current] = 0;
                                current++;
                                break;
                            }
                        }
                    }
                    labels[current] = j;
                    int default_value = 0;
                    if (base->types[labels[current]] == OP_SCALE) {
                        default_value = 128;
                    }
                    if ((flags & 0x1) == 0) {
                        x[current] = default_value;
                    } else {
                        x[current] = gsmart(&tran2);
                    }
                    if ((flags & 0x2) == 0) {
                        y[current] = default_value;
                    } else {
                        y[current] = gsmart(&tran2);
                    }
                    if ((flags & 0x4) == 0) {
                        z[current] = default_value;
                    } else {
                        z[current] = gsmart(&tran2);
                    }
                    last_group = j;
                    current++;
                }
            }
            frame->length = current;
            frame->groups = calloc(current, sizeof(int));
            frame->x = calloc(current, sizeof(int));
            frame->y = calloc(current, sizeof(int));
            frame->z = calloc(current, sizeof(int));
            for (flags = 0; flags < current; flags++) {
                frame->groups[flags] = labels[flags];
                frame->x[flags] = x[flags];
                frame->y[flags] = y[flags];
                frame->z[flags] = z[flags];
            }
        }

        free(data);
    }

    free(labels);
    free(x);
    free(y);
    free(z);
}
