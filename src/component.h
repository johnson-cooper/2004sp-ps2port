#pragma once

#include <stdbool.h>

#include "datastruct/lrucache.h"
#include "defines.h"
#include "model.h"
#include "pix24.h"
#include "pixfont.h"

#define TYPE_LAYER 0
#define TYPE_UNUSED 1 // TODO: decodes g2, gbool, center, font, shadowed, colour
#define TYPE_INV 2
#define TYPE_RECT 3
#define TYPE_TEXT 4
#define TYPE_GRAPHIC 5
#define TYPE_MODEL 6
#define TYPE_INV_TEXT 7
#define BUTTON_OK 1
#define BUTTON_TARGET 2
#define BUTTON_CLOSE 3
#define BUTTON_TOGGLE 4
#define BUTTON_SELECT 5
#define BUTTON_CONTINUE 6

typedef struct {
    int *invSlotObjId;
    int *invSlotObjCount;
    int seqFrame;
    int seqCycle;
    int id;
    int layer;
    int type;
    int buttonType;
    int clientCode;

    /* Client codes:
     * ---- friends
     * 1-200: friends list
     * 201: add friend
     * 202: delete friend
     * 203: friends list scrollbar size
     * ---- logout
     * 205: logout
     * ---- player_design
     * 300: change head (left)
     * 301: change head (right)
     * 302: change jaw (left)
     * 303: change jaw (right)
     * 304: change torso (left)
     * 305: change torso (right)
     * 306: change arms (left)
     * 307: change arms (right)
     * 308: change hands (left)
     * 309: change hands (right)
     * 310: change legs (left)
     * 311: change legs (right)
     * 312: change feet (left)
     * 313: change feet (right)
     * 314: recolour hair (left)
     * 315: recolour hair (right)
     * 316: recolour torso (left)
     * 317: recolour torso (right)
     * 318: recolour legs (left)
     * 319: recolour legs (right)
     * 320: recolour feet (left)
     * 321: recolour feet (right)
     * 322: recolour skin (left)
     * 323: recolour skin (right)
     * 324: switch to male
     * 325: switch to female
     * 326: accept design
     * 327: design preview
     * ---- ignore
     * 401-500: ignore list
     * 501: add ignore
     * 502: delete ignore
     * 503: ignore list scrollbar size
     * ---- reportabuse
     * 601: rule 1
     * 602: rule 2
     * 603: rule 3
     * 604: rule 4
     * 605: rule 5
     * 606: rule 6
     * 607: rule 7
     * 608: rule 8
     * 609: rule 9
     * 610: rule 10
     * 611: rule 11
     * 612: rule 12
     * 613: moderator mute
     * ---- welcome_screen / welcome_screen2
     * 650: last login info (has recovery questions set)
     * 651: unread messages
     * 655: last login info (no recovery questions set)
     */

    int width;
    int height;
    int trans; // rev254 adds this byte between height and overLayer - was missing entirely,
               // desyncing every component's decode by 1 byte (see component_unpack)
    int x;
    int y;
    int **scripts;
    int *scriptComparator;
    int *scriptOperand;
    int overLayer;
    int scroll;
    int scrollPosition;
    bool hide;
    int *childId;
    int *childX;
    int *childY;
    int unusedShort1;
    bool unusedBoolean1;
    bool draggable;
    bool interactable;
    bool usable;
    bool objReplace;
    int marginX;
    int marginY;
    Pix24 **invSlotSprite;
    int *invSlotOffsetX;
    int *invSlotOffsetY;
    char **iops;
    bool fill;
    bool center;
    bool shadowed;
    PixFont *font;
    // allocated lazily (DOUBLE_STR bytes, only for components that actually use it - TYPE_TEXT at
    // decode time, or client_update_interface_content()'s runtime text updates) rather than a fixed
    // inline array every one of ~8462 real components paid for regardless of type - real memory
    // pressure on PS2's 32MB, confirmed via mallinfo().
    char *text;
    char *activeText;
    int colour;
    int activeColour;
    int overColour;
    int activeOverColour;
    Pix24 *graphic;
    Pix24 *activeGraphic;
    Model *model;
    Model *activeModel;
    // Dynamic packet models are owned by this component; archive/cache models
    // remain shared and must never be freed here.
    bool modelOwned;
#ifdef __PS2__
    // lazy decode: component_unpack() stores raw sprite/model ids here instead of eagerly calling
    // component_get_image()/component_get_model() for every interface component at login - most
    // interfaces (bank, quest journal, minigame screens, etc.) are never opened in a given session,
    // and PS2's 32MB can't afford holding all of them decoded for the whole session regardless.
    // graphic/activeGraphic/model/activeModel above are populated on first real use instead, via
    // component_ensure_graphic()/component_ensure_model().
    char *graphicSpriteName;
    int graphicSpriteId;
    char *activeGraphicSpriteName;
    int activeGraphicSpriteId;
    // heap-allocated (like invSlotSprite itself), not a fixed [20] inline array - only TYPE_INV
    // components need these, and a fixed array would cost every one of the ~8462 real components
    // regardless of type (see the text/option comment above for the same reasoning)
    char **invSlotSpriteName;
    int *invSlotSpriteId;
    int modelId; // -1 = none (0 is a valid real model id, so can't double as the sentinel)
    int activeModelId;
#endif
    int anim;
    int activeAnim;
    int zoom;
    int xan;
    int yan;
    char *actionVerb;
    char *action;
    int actionTarget;
    // allocated lazily (HALF_STR bytes) same reasoning as text above - only BUTTON_OK/TOGGLE/
    // SELECT/CONTINUE components ever have a real option string.
    char *option;

    int childCount;
    int comparatorCount;
    int scriptCount;
} Component;

typedef struct {
    int count;
    Component **instances;
    LruCache *imageCache;
    LruCache *modelCache;
#ifdef __PS2__
    Jagfile *media; // kept resident (not freed after component_unpack) for lazy graphic decode
#endif
} ComponentData;

void component_free_global(void);
void component_unpack(Jagfile *jag, Jagfile *media, PixFont **fonts);
Pix24 *component_get_image(Jagfile *media, char *sprite, int spriteId);
Model *component_get_model(int id);
Model *component_get_model2(Component *com, int primaryFrame, int secondaryFrame, bool active, bool *_free);
#ifdef __PS2__
void component_ensure_graphic(Component *com);
void component_ensure_model(Component *com);
void component_ensure_invslot_sprite(Component *com, int slot);
#endif
void component_set_dynamic_model(Component *com, Model *model);
