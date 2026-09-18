#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __PS2__
#include <malloc.h>
#endif

#include "component.h"
#include "datastruct/jstring.h"
#include "datastruct/lrucache.h"
#include "jagfile.h"
#include "model.h"
#include "platform.h"

ComponentData _Component = {0};

void component_free_global(void) {
    for (int i = 0; i < _Component.count; i++) {
        if (_Component.instances[i]) {
            for (int j = 0; j < _Component.instances[i]->scriptCount; j++) {
                free(_Component.instances[i]->scripts[j]);
            }
            free(_Component.instances[i]->scripts);
            if (_Component.instances[i]->iops) {
                for (int j = 0; j < 5; j++) {
                    free(_Component.instances[i]->iops[j]);
                }
                free(_Component.instances[i]->iops);
            }
            free(_Component.instances[i]->childId);
            free(_Component.instances[i]->childX);
            free(_Component.instances[i]->childY);
            free(_Component.instances[i]->scriptComparator);
            free(_Component.instances[i]->scriptOperand);
            if (_Component.instances[i]->actionVerb) {
                free(_Component.instances[i]->actionVerb);
                free(_Component.instances[i]->action);
            }
            if (_Component.instances[i]->activeText) {
                free(_Component.instances[i]->activeText);
            }
            // TODO can't free these due to them being modified in packets
            if (_Component.instances[i]->invSlotObjCount) {
                // for (int j = 0; j < 20; j++) {
                //     if (_Component.instances[i]->invSlotSprite && _Component.instances[i]->invSlotSprite[j]) {
                //         pix24_free(_Component.instances[i]->invSlotSprite[j]);
                //     }
                // }
                free(_Component.instances[i]->invSlotOffsetX);
                free(_Component.instances[i]->invSlotOffsetY);
                free(_Component.instances[i]->invSlotSprite);
                free(_Component.instances[i]->invSlotObjId);
                free(_Component.instances[i]->invSlotObjCount);
            }
            if (_Component.instances[i]->modelOwned) {
                model_free(_Component.instances[i]->model);
            }
            // if (_Component.instances[i]->activeModel) {
            //     model_free(_Component.instances[i]->activeModel);
            // }
            // if (_Component.instances[i]->graphic) {
            //     pix24_free(_Component.instances[i]->graphic);
            // }
            // if (_Component.instances[i]->activeGraphic) {
            //     pix24_free(_Component.instances[i]->activeGraphic);
            // }
#ifdef __PS2__
            // unlike model/graphic above, these are plain metadata strings never touched by
            // packets, so freeing them here is safe
            free(_Component.instances[i]->graphicSpriteName);
            free(_Component.instances[i]->activeGraphicSpriteName);
            if (_Component.instances[i]->invSlotSpriteName) {
                for (int j = 0; j < 20; j++) {
                    free(_Component.instances[i]->invSlotSpriteName[j]);
                }
                free(_Component.instances[i]->invSlotSpriteName);
            }
            free(_Component.instances[i]->invSlotSpriteId);
#endif
        }
        free(_Component.instances[i]);
    }
    free(_Component.instances);
#ifdef __PS2__
    free(_Component.interfaceData);
    free(_Component.interfaceOffsets);
    free(_Component.interfaceLayers);
    free(_Component.interfaceClientCodes);
#endif
}

#ifdef __PS2__
static void component_skip_jstr(Packet *dat) {
    while (dat->pos < dat->length && dat->data[dat->pos++] != 10) {
    }
}

static bool component_index_record(Packet *dat, int id, int layer) {
    if (id < 0 || id >= _Component.count || dat->pos >= dat->length) return false;
    _Component.interfaceOffsets[id] = dat->pos;
    _Component.interfaceLayers[id] = layer;

    int type = g1(dat);
    int buttonType = g1(dat);
    _Component.interfaceClientCodes[id] = g2(dat);
    int width = g2(dat);
    int height = g2(dat);
    (void)g1(dat);
    int overLayer = g1(dat);
    if (overLayer != 0) (void)g1(dat);

    int comparatorCount = g1(dat);
    for (int i = 0; i < comparatorCount; i++) { (void)g1(dat); (void)g2(dat); }
    int scriptCount = g1(dat);
    for (int i = 0; i < scriptCount; i++) {
        int opcodeCount = g2(dat);
        for (int j = 0; j < opcodeCount; j++) (void)g2(dat);
    }

    if (type == TYPE_LAYER) {
        (void)g2(dat); (void)g1(dat);
        int childCount = g2(dat);
        for (int i = 0; i < childCount; i++) { (void)g2(dat); (void)g2b(dat); (void)g2b(dat); }
    }
    if (type == TYPE_UNUSED) { (void)g2(dat); (void)g1(dat); }
    if (type == TYPE_INV) {
        (void)g1(dat); (void)g1(dat); (void)g1(dat); (void)g1(dat); (void)g1(dat); (void)g1(dat);
        for (int i = 0; i < 20; i++) {
            if (g1(dat) == 1) { (void)g2b(dat); (void)g2b(dat); component_skip_jstr(dat); }
        }
        for (int i = 0; i < 5; i++) component_skip_jstr(dat);
    }
    if (type == TYPE_RECT) (void)g1(dat);
    if (type == TYPE_TEXT || type == TYPE_UNUSED) { (void)g1(dat); (void)g1(dat); (void)g1(dat); }
    if (type == TYPE_TEXT) { component_skip_jstr(dat); component_skip_jstr(dat); }
    if (type == TYPE_UNUSED || type == TYPE_RECT || type == TYPE_TEXT) (void)g4(dat);
    if (type == TYPE_RECT || type == TYPE_TEXT) { (void)g4(dat); (void)g4(dat); (void)g4(dat); }
    if (type == TYPE_GRAPHIC) { component_skip_jstr(dat); component_skip_jstr(dat); }
    if (type == TYPE_MODEL) {
        int tmp = g1(dat); if (tmp != 0) (void)g1(dat);
        tmp = g1(dat); if (tmp != 0) (void)g1(dat);
        tmp = g1(dat); if (tmp != 0) (void)g1(dat);
        tmp = g1(dat); if (tmp != 0) (void)g1(dat);
        (void)g2(dat); (void)g2(dat); (void)g2(dat);
    }
    if (type == TYPE_INV_TEXT) {
        (void)g1(dat); (void)g1(dat); (void)g1(dat); (void)g4(dat);
        (void)g2b(dat); (void)g2b(dat); (void)g1(dat);
        for (int i = 0; i < 5; i++) component_skip_jstr(dat);
    }
    if (buttonType == BUTTON_TARGET || type == TYPE_INV) {
        component_skip_jstr(dat); component_skip_jstr(dat); (void)g2(dat);
    }
    if (buttonType == BUTTON_OK || buttonType == BUTTON_TOGGLE ||
        buttonType == BUTTON_SELECT || buttonType == BUTTON_CONTINUE) component_skip_jstr(dat);
    return dat->pos <= dat->length;
}

static Component *component_decode_lazy(int id) {
    if (id < 0 || id >= _Component.count || !_Component.interfaceOffsets ||
        _Component.interfaceOffsets[id] < 0 || !_Component.interfaceData) return NULL;
    if (_Component.instances[id]) return _Component.instances[id];

    Packet dat = {0};
    dat.data = _Component.interfaceData;
    dat.length = _Component.interfaceDataLength;
    dat.pos = _Component.interfaceOffsets[id];

    Component *com = calloc(1, sizeof(Component));
    if (!com) return NULL;
    com->id = id;
    com->layer = _Component.interfaceLayers[id];
    com->modelId = -1;
    com->activeModelId = -1;

    com->type = g1(&dat);
    com->buttonType = g1(&dat);
    com->clientCode = g2(&dat);
    com->width = g2(&dat);
    com->height = g2(&dat);
    com->trans = g1(&dat);
    com->overLayer = g1(&dat);
    if (com->overLayer == 0) com->overLayer = -1;
    else com->overLayer = ((com->overLayer - 1) << 8) + g1(&dat);

    com->comparatorCount = g1(&dat);
    if (com->comparatorCount > 0) {
        com->scriptComparator = calloc(com->comparatorCount, sizeof(int));
        com->scriptOperand = calloc(com->comparatorCount, sizeof(int));
        if (!com->scriptComparator || !com->scriptOperand) goto fail;
        for (int i = 0; i < com->comparatorCount; i++) {
            com->scriptComparator[i] = g1(&dat);
            com->scriptOperand[i] = g2(&dat);
        }
    }
    com->scriptCount = g1(&dat);
    if (com->scriptCount > 0) {
        com->scripts = calloc(com->scriptCount, sizeof(int *));
        if (!com->scripts) goto fail;
        for (int i = 0; i < com->scriptCount; i++) {
            int opcodeCount = g2(&dat);
            com->scripts[i] = calloc(opcodeCount, sizeof(int));
            if (!com->scripts[i]) goto fail;
            for (int j = 0; j < opcodeCount; j++) com->scripts[i][j] = g2(&dat);
        }
    }
    if (com->type == TYPE_LAYER) {
        com->scroll = g2(&dat);
        com->hide = g1(&dat) == 1;
        com->childCount = g2(&dat);
        com->childId = calloc(com->childCount, sizeof(int));
        com->childX = calloc(com->childCount, sizeof(int));
        com->childY = calloc(com->childCount, sizeof(int));
        if (com->childCount > 0 && (!com->childId || !com->childX || !com->childY)) goto fail;
        for (int i = 0; i < com->childCount; i++) {
            com->childId[i] = g2(&dat); com->childX[i] = g2b(&dat); com->childY[i] = g2b(&dat);
        }
    }
    if (com->type == TYPE_UNUSED) { com->unusedShort1 = g2(&dat); com->unusedBoolean1 = g1(&dat) == 1; }
    if (com->type == TYPE_INV) {
        int slots = com->width * com->height;
        com->invSlotObjId = calloc(slots, sizeof(int));
        com->invSlotObjCount = calloc(slots, sizeof(int));
        com->draggable = g1(&dat) == 1; com->interactable = g1(&dat) == 1;
        com->usable = g1(&dat) == 1; com->objReplace = g1(&dat) == 1;
        com->marginX = g1(&dat); com->marginY = g1(&dat);
        com->invSlotOffsetX = calloc(20, sizeof(int)); com->invSlotOffsetY = calloc(20, sizeof(int));
        com->invSlotSprite = calloc(20, sizeof(Pix24 *));
        com->invSlotSpriteName = calloc(20, sizeof(char *)); com->invSlotSpriteId = calloc(20, sizeof(int));
        if ((slots > 0 && (!com->invSlotObjId || !com->invSlotObjCount)) ||
            !com->invSlotOffsetX || !com->invSlotOffsetY || !com->invSlotSprite ||
            !com->invSlotSpriteName || !com->invSlotSpriteId) goto fail;
        for (int i = 0; i < 20; i++) {
            if (g1(&dat) == 1) {
                com->invSlotOffsetX[i] = g2b(&dat); com->invSlotOffsetY[i] = g2b(&dat);
                char *sprite = gjstr(&dat); size_t len = strlen(sprite); char *comma = strrchr(sprite, ',');
                if (_Component.media && len > 0 && comma) {
                    int sprite_index = (int)(comma - sprite);
                    com->invSlotSpriteName[i] = substring(sprite, 0, sprite_index);
                    char *sprite_id = substring(sprite, sprite_index + 1, len);
                    com->invSlotSpriteId[i] = atoi(sprite_id); free(sprite_id);
                }
                free(sprite);
            }
        }
        com->iops = calloc(5, sizeof(char *));
        if (!com->iops) goto fail;
        for (int i = 0; i < 5; i++) {
            com->iops[i] = gjstr(&dat);
            if (strlen(com->iops[i]) == 0) { free(com->iops[i]); com->iops[i] = NULL; }
        }
    }
    if (com->type == TYPE_RECT) com->fill = g1(&dat) == 1;
    if (com->type == TYPE_TEXT || com->type == TYPE_UNUSED) {
        com->center = g1(&dat) == 1;
        int fontId = g1(&dat); if (fontId >= 0 && fontId < 4) com->font = _Component.fonts[fontId];
        com->shadowed = g1(&dat) == 1;
    }
    if (com->type == TYPE_TEXT) {
        char *text = gjstr(&dat); com->text = malloc(DOUBLE_STR);
        if (!com->text) { free(text); goto fail; }
        strncpy(com->text, text, DOUBLE_STR - 1); com->text[DOUBLE_STR - 1] = '\0'; free(text);
        com->activeText = gjstr(&dat);
    }
    if (com->type == TYPE_UNUSED || com->type == TYPE_RECT || com->type == TYPE_TEXT) com->colour = g4(&dat);
    if (com->type == TYPE_RECT || com->type == TYPE_TEXT) {
        com->activeColour = g4(&dat); com->overColour = g4(&dat); com->activeOverColour = g4(&dat);
    }
    if (com->type == TYPE_GRAPHIC) {
        char *sprite = gjstr(&dat); size_t len = strlen(sprite); char *comma = strrchr(sprite, ',');
        if (_Component.media && len > 0 && comma) {
            int sprite_index = (int)(comma - sprite); com->graphicSpriteName = substring(sprite, 0, sprite_index);
            char *sprite_id = substring(sprite, sprite_index + 1, len); com->graphicSpriteId = atoi(sprite_id); free(sprite_id);
        }
        free(sprite);
        sprite = gjstr(&dat); len = strlen(sprite); comma = strrchr(sprite, ',');
        if (_Component.media && len > 0 && comma) {
            int sprite_index = (int)(comma - sprite); com->activeGraphicSpriteName = substring(sprite, 0, sprite_index);
            char *sprite_id = substring(sprite, sprite_index + 1, len); com->activeGraphicSpriteId = atoi(sprite_id); free(sprite_id);
        }
        free(sprite);
    }
    if (com->type == TYPE_MODEL) {
        int tmp = g1(&dat); if (tmp != 0) com->modelId = ((tmp - 1) << 8) + g1(&dat);
        tmp = g1(&dat); if (tmp != 0) com->activeModelId = ((tmp - 1) << 8) + g1(&dat);
        tmp = g1(&dat); com->anim = tmp == 0 ? -1 : ((tmp - 1) << 8) + g1(&dat);
        tmp = g1(&dat); com->activeAnim = tmp == 0 ? -1 : ((tmp - 1) << 8) + g1(&dat);
        com->zoom = g2(&dat); com->xan = g2(&dat); com->yan = g2(&dat);
    }
    if (com->type == TYPE_INV_TEXT) {
        int slots = com->width * com->height;
        com->invSlotObjId = calloc(slots, sizeof(int)); com->invSlotObjCount = calloc(slots, sizeof(int));
        com->center = g1(&dat) == 1;
        int fontId = g1(&dat); if (fontId >= 0 && fontId < 4) com->font = _Component.fonts[fontId];
        com->shadowed = g1(&dat) == 1; com->colour = g4(&dat);
        com->marginX = g2b(&dat); com->marginY = g2b(&dat); com->interactable = g1(&dat) == 1;
        com->iops = calloc(5, sizeof(char *));
        if ((slots > 0 && (!com->invSlotObjId || !com->invSlotObjCount)) || !com->iops) goto fail;
        for (int i = 0; i < 5; i++) {
            com->iops[i] = gjstr(&dat);
            if (strlen(com->iops[i]) == 0) { free(com->iops[i]); com->iops[i] = NULL; }
        }
    }
    if (com->buttonType == BUTTON_TARGET || com->type == TYPE_INV) {
        com->actionVerb = gjstr(&dat); com->action = gjstr(&dat); com->actionTarget = g2(&dat);
    }
    if (com->buttonType == BUTTON_OK || com->buttonType == BUTTON_TOGGLE ||
        com->buttonType == BUTTON_SELECT || com->buttonType == BUTTON_CONTINUE) {
        char *option = gjstr(&dat); com->option = malloc(HALF_STR);
        if (!com->option) { free(option); goto fail; }
        strncpy(com->option, option, HALF_STR - 1); com->option[HALF_STR - 1] = '\0';
        if (strlen(com->option) == 0) {
            if (com->buttonType == BUTTON_OK) strcpy(com->option, "Ok");
            else if (com->buttonType == BUTTON_TOGGLE) strcpy(com->option, "Select");
            else if (com->buttonType == BUTTON_SELECT) strcpy(com->option, "Select");
            else if (com->buttonType == BUTTON_CONTINUE) strcpy(com->option, "Continue");
        }
        free(option);
    }

    _Component.instances[id] = com; _Component.loadedCount++; return com;

fail:
    if (com->scripts) for (int i = 0; i < com->scriptCount; i++) free(com->scripts[i]);
    free(com->scripts); free(com->scriptComparator); free(com->scriptOperand);
    free(com->childId); free(com->childX); free(com->childY);
    if (com->iops) for (int i = 0; i < 5; i++) free(com->iops[i]);
    free(com->iops); free(com->invSlotObjId); free(com->invSlotObjCount);
    free(com->invSlotOffsetX); free(com->invSlotOffsetY); free(com->invSlotSprite);
    if (com->invSlotSpriteName) for (int i = 0; i < 20; i++) free(com->invSlotSpriteName[i]);
    free(com->invSlotSpriteName); free(com->invSlotSpriteId);
    free(com->text); free(com->activeText); free(com->graphicSpriteName); free(com->activeGraphicSpriteName);
    free(com->actionVerb); free(com->action); free(com->option); free(com); return NULL;
}
#endif

bool component_exists(int id) {
    if (id < 0 || id >= _Component.count) return false;
#ifdef __PS2__
    return _Component.interfaceOffsets && _Component.interfaceOffsets[id] >= 0;
#else
    return _Component.instances && _Component.instances[id] != NULL;
#endif
}
Component *component_get_by_id(int id) {
    if (!component_exists(id)) return NULL;
#ifdef __PS2__
    return _Component.instances[id] ? _Component.instances[id] : component_decode_lazy(id);
#else
    return _Component.instances[id];
#endif
}
Component *component_find_by_client_code(int clientCode) {
#ifdef __PS2__
    if (_Component.interfaceClientCodes) {
        for (int i = 0; i < _Component.count; i++)
            if (_Component.interfaceOffsets[i] >= 0 && _Component.interfaceClientCodes[i] == clientCode)
                return component_get_by_id(i);
        return NULL;
    }
#endif
    for (int i = 0; i < _Component.count; i++) {
        Component *com = _Component.instances ? _Component.instances[i] : NULL;
        if (com && com->clientCode == clientCode) return com;
    }
    return NULL;
}

void component_unpack(Jagfile *jag, Jagfile *media, PixFont **fonts) {
#ifdef __PS2__
    _Component.imageCache = lrucache_new(50000);
    _Component.modelCache = lrucache_new(50000);
    _Component.media = media;
    for (int i = 0; i < 4; i++) _Component.fonts[i] = fonts ? fonts[i] : NULL;
    Packet *lazy_dat = jagfile_to_packet(jag, "data");
    if (!lazy_dat) return;
    _Component.count = g2(lazy_dat);
    _Component.instances = calloc(_Component.count, sizeof(Component *));
    _Component.interfaceOffsets = malloc(_Component.count * sizeof(int));
    _Component.interfaceLayers = malloc(_Component.count * sizeof(int));
    _Component.interfaceClientCodes = malloc(_Component.count * sizeof(int));
    if (!_Component.instances || !_Component.interfaceOffsets || !_Component.interfaceLayers ||
        !_Component.interfaceClientCodes) { packet_free(lazy_dat); return; }
    for (int i = 0; i < _Component.count; i++) {
        _Component.interfaceOffsets[i] = -1; _Component.interfaceLayers[i] = -1; _Component.interfaceClientCodes[i] = -1;
    }
    int layer = -1, indexed = 0;
    while (lazy_dat->pos < lazy_dat->length) {
        int id = g2(lazy_dat);
        if (id == 65535) { layer = g2(lazy_dat); id = g2(lazy_dat); }
        if (!component_index_record(lazy_dat, id, layer)) {
            rs2_error("component lazy index failed at id=%d pos=%d/%d\n", id, lazy_dat->pos, lazy_dat->length);
            break;
        }
        indexed++;
    }
    _Component.interfaceData = lazy_dat->data;
    _Component.interfaceDataLength = lazy_dat->length;
    lazy_dat->data = NULL;
    packet_free(lazy_dat);
    _Component.loadedCount = 0;
    rs2_log("PS2 interfaces indexed: definitions=%d count=%d raw=%d bytes loaded=%d\n",
            indexed, _Component.count, _Component.interfaceDataLength, _Component.loadedCount);
    return;
#else
    _Component.imageCache = lrucache_new(50000);
    _Component.modelCache = lrucache_new(50000);
#ifdef __PS2__
    // kept resident (see component.h) so component_ensure_graphic() can still decode from it after
    // this function returns and the caller frees its own local `media` reference
    _Component.media = media;
#endif

    Packet *dat = jagfile_to_packet(jag, "data");
    int layer = -1;

    _Component.count = g2(dat);
    _Component.instances = calloc(_Component.count, sizeof(Component *));

    while (dat->pos < dat->length) {
        int id = g2(dat);
        if (id == 65535) {
            layer = g2(dat);
            id = g2(dat);
        }
#ifdef __PS2__
        if (id % 500 == 0) {
            rs2_log("MEM component_unpack id=%d: used=%d free=%d\n", id, mallinfo().uordblks, mallinfo().fordblks);
        }
#endif

        Component *com = _Component.instances[id] = calloc(1, sizeof(Component));
        com->id = id;
        com->layer = layer;
#ifdef __PS2__
        // calloc zeroes these to 0, but 0 is a valid real model id - must set the "none" sentinel
        // explicitly so component_ensure_model() doesn't try to decode a bogus id 0 for every
        // non-TYPE_MODEL component
        com->modelId = -1;
        com->activeModelId = -1;
#endif
        com->type = g1(dat);
        com->buttonType = g1(dat);
        com->clientCode = g2(dat);
        com->width = g2(dat);
        com->height = g2(dat);
        com->trans = g1(dat);
        com->overLayer = g1(dat);
        if (com->overLayer == 0) {
            com->overLayer = -1;
        } else {
            com->overLayer = ((com->overLayer - 1) << 8) + g1(dat);
        }

        com->comparatorCount = g1(dat);
        if (com->comparatorCount > 0) {
            com->scriptComparator = calloc(com->comparatorCount, sizeof(int));
            com->scriptOperand = calloc(com->comparatorCount, sizeof(int));

            for (int i = 0; i < com->comparatorCount; i++) {
                com->scriptComparator[i] = g1(dat);
                com->scriptOperand[i] = g2(dat);
            }
        }

        com->scriptCount = g1(dat);
        if (com->scriptCount > 0) {
            com->scripts = calloc(com->scriptCount, sizeof(int *));

            for (int i = 0; i < com->scriptCount; i++) {
                int opcodeCount = g2(dat);
                com->scripts[i] = calloc(opcodeCount, sizeof(int));

                for (int j = 0; j < opcodeCount; j++) {
                    com->scripts[i][j] = g2(dat);
                }
            }
        }

        if (com->type == TYPE_LAYER) {
            com->scroll = g2(dat);
            com->hide = g1(dat) == 1;

            com->childCount = g2(dat);
            com->childId = calloc(com->childCount, sizeof(int));
            com->childX = calloc(com->childCount, sizeof(int));
            com->childY = calloc(com->childCount, sizeof(int));

            for (int i = 0; i < com->childCount; i++) {
                com->childId[i] = g2(dat);
                com->childX[i] = g2b(dat);
                com->childY[i] = g2b(dat);
            }
        }

        if (com->type == TYPE_UNUSED) {
            com->unusedShort1 = g2(dat);
            com->unusedBoolean1 = g1(dat) == 1;
        }

        if (com->type == TYPE_INV) {
            com->invSlotObjId = calloc(com->width * com->height, sizeof(int));
            com->invSlotObjCount = calloc(com->width * com->height, sizeof(int));

            com->draggable = g1(dat) == 1;
            com->interactable = g1(dat) == 1;
            com->usable = g1(dat) == 1;
            com->objReplace = g1(dat) == 1;
            com->marginX = g1(dat);
            com->marginY = g1(dat);

            com->invSlotOffsetX = calloc(20, sizeof(int));
            com->invSlotOffsetY = calloc(20, sizeof(int));
            com->invSlotSprite = calloc(20, sizeof(Pix24 *));
#ifdef __PS2__
            com->invSlotSpriteName = calloc(20, sizeof(char *));
            com->invSlotSpriteId = calloc(20, sizeof(int));
#endif

            for (int i = 0; i < 20; i++) {
                if (g1(dat) == 1) {
                    com->invSlotOffsetX[i] = g2b(dat);
                    com->invSlotOffsetY[i] = g2b(dat);

                    char *sprite = gjstr(dat);
                    size_t len = strlen(sprite);
                    char *comma = strrchr(sprite, ',');
                    if (media && len > 0 && comma) {
                        int sprite_index = (int)(comma - sprite);
                        char *sprite_name = substring(sprite, 0, sprite_index);
                        char *sprite_id = substring(sprite, sprite_index + 1, len);
#ifdef __PS2__
                        com->invSlotSpriteName[i] = sprite_name;
                        com->invSlotSpriteId[i] = atoi(sprite_id);
                        free(sprite_id);
#else
                        com->invSlotSprite[i] = component_get_image(media, sprite_name, atoi(sprite_id));
                        free(sprite_name);
                        free(sprite_id);
#endif
                    }
                    free(sprite);
                }
            }

            com->iops = calloc(5, sizeof(char *));
            for (int i = 0; i < 5; i++) {
                com->iops[i] = gjstr(dat);

                if (strlen(com->iops[i]) == 0) {
                    free(com->iops[i]);
                    com->iops[i] = NULL;
                }
            }
        }

        if (com->type == TYPE_RECT) {
            com->fill = g1(dat) == 1;
        }

        if (com->type == TYPE_TEXT || com->type == TYPE_UNUSED) {
            com->center = g1(dat) == 1;
            int fontId = g1(dat);
            if (fonts && fontId >= 0 && fontId < 4) {
                com->font = fonts[fontId];
            }
            com->shadowed = g1(dat) == 1;
        }

        if (com->type == TYPE_TEXT) {
            char *text = gjstr(dat);
            com->text = malloc(DOUBLE_STR);
            strncpy(com->text, text, DOUBLE_STR - 1);
            com->text[DOUBLE_STR - 1] = '\0';
            free(text);
            com->activeText = gjstr(dat);
        }

        if (com->type == TYPE_UNUSED || com->type == TYPE_RECT || com->type == TYPE_TEXT) {
            com->colour = g4(dat);
        }

        if (com->type == TYPE_RECT || com->type == TYPE_TEXT) {
            com->activeColour = g4(dat);
            com->overColour = g4(dat);
            com->activeOverColour = g4(dat);
        }

        if (com->type == TYPE_GRAPHIC) {
            char *sprite = gjstr(dat);
            size_t len = strlen(sprite);
            char *comma = strrchr(sprite, ',');
            if (media && len > 0 && comma) {
                int sprite_index = (int)(comma - sprite);
                char *sprite_name = substring(sprite, 0, sprite_index);
                char *sprite_id = substring(sprite, sprite_index + 1, len);
#ifdef __PS2__
                com->graphicSpriteName = sprite_name;
                com->graphicSpriteId = atoi(sprite_id);
                free(sprite_id);
#else
                com->graphic = component_get_image(media, sprite_name, atoi(sprite_id));
                free(sprite_name);
                free(sprite_id);
#endif
            }
            free(sprite);

            sprite = gjstr(dat);
            len = strlen(sprite);
            comma = strrchr(sprite, ',');
            if (media && len > 0 && comma) {
                int sprite_index = (int)(comma - sprite);
                char *sprite_name = substring(sprite, 0, sprite_index);
                char *sprite_id = substring(sprite, sprite_index + 1, len);
#ifdef __PS2__
                com->activeGraphicSpriteName = sprite_name;
                com->activeGraphicSpriteId = atoi(sprite_id);
                free(sprite_id);
#else
                com->activeGraphic = component_get_image(media, sprite_name, atoi(sprite_id));
                free(sprite_name);
                free(sprite_id);
#endif
            }
            free(sprite);
        }

        if (com->type == TYPE_MODEL) {
            int tmp = g1(dat);
            if (tmp != 0) {
#ifdef __PS2__
                com->modelId = ((tmp - 1) << 8) + g1(dat);
#else
                com->model = component_get_model(((tmp - 1) << 8) + g1(dat));
#endif
            }

            tmp = g1(dat);
            if (tmp != 0) {
#ifdef __PS2__
                com->activeModelId = ((tmp - 1) << 8) + g1(dat);
#else
                com->activeModel = component_get_model(((tmp - 1) << 8) + g1(dat));
#endif
            }

            tmp = g1(dat);
            if (tmp == 0) {
                com->anim = -1;
            } else {
                com->anim = ((tmp - 1) << 8) + g1(dat);
            }

            tmp = g1(dat);
            if (tmp == 0) {
                com->activeAnim = -1;
            } else {
                com->activeAnim = ((tmp - 1) << 8) + g1(dat);
            }

            com->zoom = g2(dat);
            com->xan = g2(dat);
            com->yan = g2(dat);
        }

        if (com->type == TYPE_INV_TEXT) {
            com->invSlotObjId = calloc(com->width * com->height, sizeof(int));
            com->invSlotObjCount = calloc(com->width * com->height, sizeof(int));

            com->center = g1(dat) == 1;
            int fontId = g1(dat);
            if (fonts && fontId >= 0 && fontId < 4) {
                com->font = fonts[fontId];
            }
            com->shadowed = g1(dat) == 1;
            com->colour = g4(dat);
            com->marginX = g2b(dat);
            com->marginY = g2b(dat);
            com->interactable = g1(dat) == 1;

            com->iops = calloc(5, sizeof(char *));
            for (int i = 0; i < 5; i++) {
                com->iops[i] = gjstr(dat);

                if (strlen(com->iops[i]) == 0) {
                    free(com->iops[i]);
                    com->iops[i] = NULL;
                }
            }
        }

        if (com->buttonType == BUTTON_TARGET || com->type == TYPE_INV) {
            com->actionVerb = gjstr(dat);
            com->action = gjstr(dat);
            com->actionTarget = g2(dat);
        }

        if (com->buttonType == BUTTON_OK || com->buttonType == BUTTON_TOGGLE || com->buttonType == BUTTON_SELECT || com->buttonType == BUTTON_CONTINUE) {
            char *option = gjstr(dat);
            com->option = malloc(HALF_STR);
            strncpy(com->option, option, HALF_STR - 1);
            com->option[HALF_STR - 1] = '\0';

            if (strlen(com->option) == 0) {
                if (com->buttonType == BUTTON_OK) {
                    strcpy(com->option, "Ok");
                } else if (com->buttonType == BUTTON_TOGGLE) {
                    strcpy(com->option, "Select");
                } else if (com->buttonType == BUTTON_SELECT) {
                    strcpy(com->option, "Select");
                } else if (com->buttonType == BUTTON_CONTINUE) {
                    strcpy(com->option, "Continue");
                }
            }
            free(option);
        }
    }

    packet_free(dat);
#ifndef __PS2__
    // on PS2 these stay alive for the rest of the session - component_get_image()/
    // component_get_model() are the lazy decode cache now (see component_ensure_graphic()/
    // component_ensure_model()), not just a transient dedup structure for this unpack pass
    lrucache_free(_Component.imageCache);
    lrucache_free(_Component.modelCache);
#endif
#endif
}

Pix24 *component_get_image(Jagfile *media, char *sprite, int spriteId) {
    int64_t uid = (jstring_hash_code(sprite) << 8) + (int64_t)spriteId;
    Pix24 *image = (Pix24 *)lrucache_get(_Component.imageCache, uid);
    if (image) {
        return image;
    }

    // try {
    image = pix24_from_archive(media, sprite, spriteId);
    if (image) {
        lrucache_put(_Component.imageCache, uid, &image->link);
    }
    // } catch (Exception ignored) {
    // 	return null;
    // }

#ifdef __PS2__
    static int decode_count = 0;
    decode_count++;
    if (decode_count % 50 == 0) {
        rs2_log("MEM component_get_image decode #%d (%s,%d): used=%d free=%d\n", decode_count, sprite, spriteId, mallinfo().uordblks, mallinfo().fordblks);
    }
#endif

    return image;
}

void component_set_dynamic_model(Component *com, Model *model) {
    if (com->modelOwned && com->model && com->model != model) {
        model_free(com->model);
    }
    com->model = model;
    com->modelOwned = model != NULL;
#ifdef __PS2__
    // A packet-provided model supersedes any lazily decoded archive model.
    com->modelId = -1;
#endif
}

Model *component_get_model(int id) {
    Model *m = (Model *)lrucache_get(_Component.modelCache, id);
    if (m) {
        return m;
    }

    m = model_from_id(id, false);
    lrucache_put(_Component.modelCache, id, &m->link);
#ifdef __PS2__
    static int model_decode_count = 0;
    model_decode_count++;
    if (model_decode_count % 20 == 0) {
        rs2_log("MEM component_get_model decode #%d (id=%d): used=%d free=%d\n", model_decode_count, id, mallinfo().uordblks, mallinfo().fordblks);
    }
#endif
    return m;
}

#ifdef __PS2__
void component_ensure_graphic(Component *com) {
    if (!com->graphic && com->graphicSpriteName) {
        com->graphic = component_get_image(_Component.media, com->graphicSpriteName, com->graphicSpriteId);
    }
    if (!com->activeGraphic && com->activeGraphicSpriteName) {
        com->activeGraphic = component_get_image(_Component.media, com->activeGraphicSpriteName, com->activeGraphicSpriteId);
    }
}

void component_ensure_model(Component *com) {
    if (!com->model && com->modelId != -1) {
        com->model = component_get_model(com->modelId);
    }
    if (!com->activeModel && com->activeModelId != -1) {
        com->activeModel = component_get_model(com->activeModelId);
    }
}

void component_ensure_invslot_sprite(Component *com, int slot) {
    if (!com->invSlotSprite[slot] && com->invSlotSpriteName[slot]) {
        com->invSlotSprite[slot] = component_get_image(_Component.media, com->invSlotSpriteName[slot], com->invSlotSpriteId[slot]);
    }
}
#endif

Model *component_get_model2(Component *com, int primaryFrame, int secondaryFrame, bool active, bool *_free) {
#ifdef __PS2__
    component_ensure_model(com);
#endif
    Model *m = com->model;
    if (active) {
        m = com->activeModel;
    }

    if (!m) {
        return NULL;
    }

    if (primaryFrame == -1 && secondaryFrame == -1 && !m->face_colors) {
        return m;
    }

    *_free = true;
    Model *tmp = model_share_colored(m, true, true, false, false);
    if (primaryFrame != -1 || secondaryFrame != -1) {
        model_create_label_references(tmp, false);
    }

    if (primaryFrame != -1) {
        model_apply_transform(tmp, primaryFrame);
    }

    if (secondaryFrame != -1) {
        model_apply_transform(tmp, secondaryFrame);
    }

    model_calculate_normals(tmp, 64, 768, -50, -10, -50, true, false);
    return tmp;
}
