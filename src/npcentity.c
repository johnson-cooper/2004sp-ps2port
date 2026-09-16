#include <stddef.h>
#include <stdlib.h>

#include "model.h"
#include "npcentity.h"
#include "npctype.h"
#include "spotanimtype.h"

extern SeqTypeData _SeqType;
extern SpotAnimTypeData _SpotAnimType;

static bool npcentity_seq_frame_valid(int seq_id, int frame) {
    if (seq_id < 0 || seq_id >= _SeqType.count || !_SeqType.instances || !_SeqType.instances[seq_id]) {
        return false;
    }
    SeqType *seq = _SeqType.instances[seq_id];
    return frame >= 0 && frame < seq->frameCount;
}

static SpotAnimType *npcentity_get_valid_spotanim(NpcEntity *npc) {
    int id = npc->pathing_entity.spotanimId;
    if (id < 0 || id >= _SpotAnimType.count || !_SpotAnimType.instances || !_SpotAnimType.instances[id]) {
        return NULL;
    }
    SpotAnimType *spotanim = _SpotAnimType.instances[id];
    if (!spotanim->seq || npc->pathing_entity.spotanimFrame < 0 ||
        npc->pathing_entity.spotanimFrame >= spotanim->seq->frameCount) {
        return NULL;
    }
    return spotanim;
}

NpcEntity *npcentity_new(void) {
    NpcEntity *npc = calloc(1, sizeof(NpcEntity));
    npc->pathing_entity = pathingentity_new("npc");
    return npc;
}

Model *npcentity_draw(NpcEntity *npc, int loopCycle) {
    (void)loopCycle;
    if (!npc->type) {
        return NULL;
    }

    if (npc->pathing_entity.spotanimId == -1 || npc->pathing_entity.spotanimFrame == -1) {
        return npcentity_get_sequencedmodel(npc);
    }

    // Live NPC_INFO can install a spot-animation id before the renderer sees
    // the entity. Client3 historically trusted that id/frame and indexed both
    // _SpotAnimType.instances[] and seq->frames[] unchecked. On rev254 a bad or
    // unsupported value must degrade to the normal NPC model, never turn one
    // server update into an EE out-of-bounds read.
    SpotAnimType *spotanim = npcentity_get_valid_spotanim(npc);
    if (!spotanim) {
        npc->pathing_entity.spotanimId = -1;
        npc->pathing_entity.spotanimFrame = -1;
        return npcentity_get_sequencedmodel(npc);
    }

    Model *model = npcentity_get_sequencedmodel(npc);
    if (!model) {
        return NULL;
    }

    Model *spotanim_model = spotanimtype_get_model(spotanim);
    if (!spotanim_model) {
        npc->pathing_entity.spotanimId = -1;
        npc->pathing_entity.spotanimFrame = -1;
        return model;
    }

    Model *model1 = model_share_colored(spotanim_model, true, !spotanim->animHasAlpha, false, false);
    if (!model1) {
        npc->pathing_entity.spotanimId = -1;
        npc->pathing_entity.spotanimFrame = -1;
        return model;
    }
    model_translate(model1, -npc->pathing_entity.spotanimOffset, 0, 0);
    model_create_label_references(model1, false);
    model_apply_transform(model1, spotanim->seq->frames[npc->pathing_entity.spotanimFrame]);
    model_free_label_references(model1);
    model1->label_faces = NULL;
    model1->label_vertices = NULL;

    if (spotanim->resizeh != 128 || spotanim->resizev != 128) {
        model_scale(model1, spotanim->resizeh, spotanim->resizev, spotanim->resizeh);
    }

    model_calculate_normals(model1, 64 + spotanim->ambient, 850 + spotanim->contrast, -30, -50, -30, true, false);

    Model *models[] = {model, model1};
    Model *tmp = model_from_models_bounds(models, 2);
    model_free_calculate_normals(model1);
    model_free_share_alpha(model, !npc->type->animHasAlpha);
    model_free_share_colored(model1, true, !spotanim->animHasAlpha, false);

    if (tmp && npc->type->size == 1) {
        tmp->pick_aabb = true;
    }

    return tmp;
}

Model *npcentity_get_sequencedmodel(NpcEntity *npc) {
    if (!npc || !npc->type) {
        return NULL;
    }

    if (npc->pathing_entity.primarySeqId >= 0 && npc->pathing_entity.primarySeqDelay == 0 &&
        npcentity_seq_frame_valid(npc->pathing_entity.primarySeqId, npc->pathing_entity.primarySeqFrame)) {
        SeqType *primary = _SeqType.instances[npc->pathing_entity.primarySeqId];
        int primaryTransformId = primary->frames[npc->pathing_entity.primarySeqFrame];
        int secondaryTransformId = -1;
        if (npc->pathing_entity.secondarySeqId >= 0 &&
            npc->pathing_entity.secondarySeqId != npc->pathing_entity.seqStandId &&
            npcentity_seq_frame_valid(npc->pathing_entity.secondarySeqId, npc->pathing_entity.secondarySeqFrame)) {
            secondaryTransformId = _SeqType.instances[npc->pathing_entity.secondarySeqId]->frames[npc->pathing_entity.secondarySeqFrame];
        }
        return npctype_get_sequencedmodel(npc->type, primaryTransformId, secondaryTransformId, primary->walkmerge);
    }

    int transformId = -1;
    if (npc->pathing_entity.secondarySeqId >= 0 &&
        npcentity_seq_frame_valid(npc->pathing_entity.secondarySeqId, npc->pathing_entity.secondarySeqFrame)) {
        transformId = _SeqType.instances[npc->pathing_entity.secondarySeqId]->frames[npc->pathing_entity.secondarySeqFrame];
    }

    Model *model = npctype_get_sequencedmodel(npc->type, transformId, -1, NULL);
    if (!model) {
        return NULL;
    }
    npc->pathing_entity.height = model->max_y;
    return model;
}

bool npcentity_is_visible(NpcEntity *npc) {
    return npc->type != NULL;
}
