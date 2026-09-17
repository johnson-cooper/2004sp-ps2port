/*
 * Renderer safety wrapper.
 *
 * The historical implementation is kept byte-for-byte in model_impl.inc.  We rename only its
 * three renderer entry points while including it, then provide bounded replacements below.  This
 * keeps the large model implementation easy to diff while making the fixed scratch-buffer limits
 * explicit at every insertion site.
 */
#define model_draw_simple model_draw_simple_unchecked
#define model_draw model_draw_unchecked
#define model_draw2 model_draw2_unchecked
#include "model_impl.inc"
#undef model_draw_simple
#undef model_draw
#undef model_draw2

#define MODEL_VERTEX_SCRATCH_COUNT 4096
#define MODEL_FACE_SCRATCH_COUNT 4096
#define MODEL_PRIORITY_COUNT 12
#define MODEL_PRIORITY_FACE_COUNT 2000

#ifdef __PS2__
static unsigned long ps2_dropped_depth_faces = 0;
static unsigned long ps2_dropped_priority_faces = 0;
static unsigned long ps2_dropped_invalid_faces = 0;

unsigned long model_ps2_dropped_depth_faces(void) { return ps2_dropped_depth_faces; }
unsigned long model_ps2_dropped_priority_faces(void) { return ps2_dropped_priority_faces; }
unsigned long model_ps2_dropped_invalid_faces(void) { return ps2_dropped_invalid_faces; }
#endif

static bool model_face_indices_valid(Model *m, int a, int b, int c) {
    return a >= 0 && a < m->vertex_count && a < MODEL_VERTEX_SCRATCH_COUNT &&
           b >= 0 && b < m->vertex_count && b < MODEL_VERTEX_SCRATCH_COUNT &&
           c >= 0 && c < m->vertex_count && c < MODEL_VERTEX_SCRATCH_COUNT;
}

static inline int model_pick_mouse_x(void) {
#ifdef __PS2__
    // The PS2 scene setup still carries a stale half-resolution `/2` conversion even though the
    // current software 3D target is the native 512x334 viewport. Undo that legacy conversion at the
    // model picker so the visible virtual cursor and model hit tests refer to the same pixel. This is
    // deliberately isolated from UI input; only World3D/model picking consumes these coordinates.
    return _Model.mouse_x << 1;
#else
    return _Model.mouse_x;
#endif
}

static inline int model_pick_mouse_y(void) {
#ifdef __PS2__
    return _Model.mouse_y << 1;
#else
    return _Model.mouse_y;
#endif
}

static void model_bucket_face(int depth_average, int face) {
    if (depth_average < 0 || depth_average >= MODEL_MAX_DEPTH) {
#ifdef __PS2__
        ps2_dropped_invalid_faces++;
#endif
        return;
    }
    int count = _Model.tmp_depth_face_count[depth_average];
    if (count >= MODEL_DEPTH_FACE_COUNT) {
#ifdef __PS2__
        ps2_dropped_depth_faces++;
#endif
        return;
    }
    _Model.tmp_depth_faces[depth_average][count] = face;
    _Model.tmp_depth_face_count[depth_average] = count + 1;
}

void model_draw_simple(Model *m, int pitch, int yaw, int roll, int eyePitch, int eyeX, int eyeY, int eyeZ) {
    if (!m || m->vertex_count < 0 || m->vertex_count > MODEL_VERTEX_SCRATCH_COUNT) {
#ifdef __PS2__
        ps2_dropped_invalid_faces++;
#endif
        return;
    }

    int centerX = _Pix3D.center_x;
    int centerY = _Pix3D.center_y;
    int sinPitch = _Pix3D.sin_table[pitch];
    int cosPitch = _Pix3D.cos_table[pitch];
    int sinYaw = _Pix3D.sin_table[yaw];
    int cosYaw = _Pix3D.cos_table[yaw];
    int sinRoll = _Pix3D.sin_table[roll];
    int cosRoll = _Pix3D.cos_table[roll];
    int sinEyePitch = _Pix3D.sin_table[eyePitch];
    int cosEyePitch = _Pix3D.cos_table[eyePitch];
    int midZ = (eyeY * sinEyePitch + eyeZ * cosEyePitch) >> 16;

    for (int v = 0; v < m->vertex_count; v++) {
        int x = m->vertices_x[v];
        int y = m->vertices_y[v];
        int z = m->vertices_z[v];
        int temp;
        if (roll != 0) {
            temp = (y * sinRoll + x * cosRoll) >> 16;
            y = (y * cosRoll - x * sinRoll) >> 16;
            x = temp;
        }
        if (pitch != 0) {
            temp = (y * cosPitch - z * sinPitch) >> 16;
            z = (y * sinPitch + z * cosPitch) >> 16;
            y = temp;
        }
        if (yaw != 0) {
            temp = (z * sinYaw + x * cosYaw) >> 16;
            z = (z * cosYaw - x * sinYaw) >> 16;
            x = temp;
        }
        x += eyeX;
        y += eyeY;
        z += eyeZ;
        temp = (y * cosEyePitch - z * sinEyePitch) >> 16;
        z = (y * sinEyePitch + z * cosEyePitch) >> 16;
        if (z == 0) {
#ifdef __PS2__
            ps2_dropped_invalid_faces++;
#endif
            return;
        }
        _Model.vertex_screen_z[v] = z - midZ;
        _Model.vertex_screen_x[v] = centerX + (x << 9) / z;
        _Model.vertex_screen_y[v] = centerY + (temp << 9) / z;
        if (m->textured_face_count > 0) {
            _Model.vertex_view_space_x[v] = x;
            _Model.vertex_view_space_y[v] = temp;
            _Model.vertex_view_space_z[v] = z;
        }
    }
    model_draw2(m, false, false, 0);
}

void model_draw(Model *m, int yaw, int sinCameraPitch, int cosCameraPitch, int sinCameraYaw, int cosCameraYaw, int sceneX, int sceneY, int sceneZ, int key) {
    if (!m || m->vertex_count < 0 || m->vertex_count > MODEL_VERTEX_SCRATCH_COUNT) {
#ifdef __PS2__
        ps2_dropped_invalid_faces++;
#endif
        return;
    }

    int a = (sceneZ * cosCameraYaw - sceneX * sinCameraYaw) >> 16;
    int b = (sceneY * sinCameraPitch + a * cosCameraPitch) >> 16;
    int c = m->radius * cosCameraPitch >> 16;
    int d = b + c;
    if (d <= 50 || b >= 3500) return;
    int e = (sceneZ * sinCameraYaw + sceneX * cosCameraYaw) >> 16;
    int minScreenX = (e - m->radius) << 9;
    if (minScreenX / d >= _Pix2D.center_x) return;
    int maxScreenX = (e + m->radius) << 9;
    if (maxScreenX / d <= -_Pix2D.center_x) return;
    int f = (sceneY * cosCameraPitch - a * sinCameraPitch) >> 16;
    int g = m->radius * sinCameraPitch >> 16;
    int maxScreenY = (f + g) << 9;
    if (maxScreenY / d <= -_Pix2D.center_y) return;
    int h = g + (m->max_y * cosCameraPitch >> 16);
    int minScreenY = (f - h) << 9;
    if (minScreenY / d >= _Pix2D.center_y) return;
    int i = c + (m->max_y * sinCameraPitch >> 16);
    bool project = b - i <= 50;
    bool hasInput = false;
    int cx;
    int cy;
    int yawsin;

    if (key > 0 && _Model.check_hover) {
        cx = b - c;
        if (cx <= 50) cx = 50;
        if (e > 0) {
            minScreenX /= d;
            maxScreenX /= cx;
        } else {
            maxScreenX /= d;
            minScreenX /= cx;
        }
        if (f > 0) {
            minScreenY /= d;
            maxScreenY /= cx;
        } else {
            maxScreenY /= d;
            minScreenY /= cx;
        }
        cy = model_pick_mouse_x() - _Pix3D.center_x;
        yawsin = model_pick_mouse_y() - _Pix3D.center_y;
        if (cy > minScreenX && cy < maxScreenX && yawsin > minScreenY && yawsin < maxScreenY) {
            if (m->pick_aabb) {
                if (_Model.picked_count < 1000) {
                    _Model.picked_bitsets[_Model.picked_count++] = key;
                }
            } else {
                hasInput = true;
            }
        }
    }

    cx = _Pix3D.center_x;
    cy = _Pix3D.center_y;
    yawsin = 0;
    int yawcos = 0;
    if (yaw != 0) {
        yawsin = _Pix3D.sin_table[yaw];
        yawcos = _Pix3D.cos_table[yaw];
    }
    for (int v = 0; v < m->vertex_count; v++) {
        int x = m->vertices_x[v];
        int y = m->vertices_y[v];
        int z = m->vertices_z[v];
        int temp;
        if (yaw != 0) {
            temp = (z * yawsin + x * yawcos) >> 16;
            z = (z * yawcos - x * yawsin) >> 16;
            x = temp;
        }
        x += sceneX;
        y += sceneY;
        z += sceneZ;
        temp = (z * sinCameraYaw + x * cosCameraYaw) >> 16;
        z = (z * cosCameraYaw - x * sinCameraYaw) >> 16;
        x = temp;
        temp = (y * cosCameraPitch - z * sinCameraPitch) >> 16;
        z = (y * sinCameraPitch + z * cosCameraPitch) >> 16;
        _Model.vertex_screen_z[v] = z - b;
        if (z >= 50) {
            _Model.vertex_screen_x[v] = cx + (x << 9) / z;
            _Model.vertex_screen_y[v] = cy + (temp << 9) / z;
        } else {
            _Model.vertex_screen_x[v] = -5000;
            project = true;
        }
#ifdef GL11
        if (!_Custom.use_opengl11) {
#endif
            if (project || m->textured_face_count > 0) {
                _Model.vertex_view_space_x[v] = x;
                _Model.vertex_view_space_y[v] = temp;
                _Model.vertex_view_space_z[v] = z;
            }
#ifdef GL11
        }
#endif
    }
    model_draw2(m, project, hasInput, key);
    gl_start_model(m, sceneX, sceneY, sceneZ, yaw);
}

void model_draw2(Model *m, bool projected, bool hasInput, int bitset) {
    if (!m || m->vertex_count < 0 || m->vertex_count > MODEL_VERTEX_SCRATCH_COUNT || m->face_count < 0) {
#ifdef __PS2__
        ps2_dropped_invalid_faces++;
#endif
        return;
    }

    int clear_depths = m->max_depth;
    if (clear_depths < 0) clear_depths = 0;
    if (clear_depths > MODEL_MAX_DEPTH) clear_depths = MODEL_MAX_DEPTH;
    for (int i = 0; i < clear_depths; i++) {
        _Model.tmp_depth_face_count[i] = 0;
    }

    int face_limit = m->face_count;
    if (face_limit > MODEL_FACE_SCRATCH_COUNT) face_limit = MODEL_FACE_SCRATCH_COUNT;
    for (int f = 0; f < face_limit; f++) {
        if (m->face_infos && m->face_infos[f] == -1) continue;
        int a = m->face_indices_a[f];
        int b = m->face_indices_b[f];
        int c = m->face_indices_c[f];
        if (!model_face_indices_valid(m, a, b, c)) {
#ifdef __PS2__
            ps2_dropped_invalid_faces++;
#endif
            continue;
        }
        int xa = _Model.vertex_screen_x[a];
        int xb = _Model.vertex_screen_x[b];
        int xc = _Model.vertex_screen_x[c];
        if (projected && (xa == -5000 || xb == -5000 || xc == -5000)) {
            _Model.face_near_clipped[f] = true;
            int depth_average = (_Model.vertex_screen_z[a] + _Model.vertex_screen_z[b] + _Model.vertex_screen_z[c]) / 3 + m->min_depth;
            model_bucket_face(depth_average, f);
        } else {
            if (hasInput && model_point_within_triangle(model_pick_mouse_x(), model_pick_mouse_y(),
                    _Model.vertex_screen_y[a], _Model.vertex_screen_y[b], _Model.vertex_screen_y[c], xa, xb, xc)) {
                if (_Model.picked_count < 1000) {
                    _Model.picked_bitsets[_Model.picked_count++] = bitset;
                }
                hasInput = false;
            }
            if ((xa - xb) * (_Model.vertex_screen_y[c] - _Model.vertex_screen_y[b]) -
                    (_Model.vertex_screen_y[a] - _Model.vertex_screen_y[b]) * (xc - xb) > 0) {
                _Model.face_near_clipped[f] = false;
                _Model.face_clipped_x[f] = xa >= 0 || xb >= 0 || xc >= 0 || xa <= _Pix2D.bound_x || xb <= _Pix2D.bound_x || xc <= _Pix2D.bound_x;
                int depth_average = (_Model.vertex_screen_z[a] + _Model.vertex_screen_z[b] + _Model.vertex_screen_z[c]) / 3 + m->min_depth;
                model_bucket_face(depth_average, f);
            }
        }
    }
#ifdef __PS2__
    if (m->face_count > MODEL_FACE_SCRATCH_COUNT) {
        ps2_dropped_invalid_faces += (unsigned long)(m->face_count - MODEL_FACE_SCRATCH_COUNT);
    }
#endif

    if (!m->face_priorities) {
        for (int depth = clear_depths - 1; depth >= 0; depth--) {
            int count = _Model.tmp_depth_face_count[depth];
            int *faces = _Model.tmp_depth_faces[depth];
            for (int f = 0; f < count; f++) model_draw_face(m, faces[f]);
        }
        return;
    }

    for (int priority = 0; priority < MODEL_PRIORITY_COUNT; priority++) {
        _Model.tmp_priority_face_count[priority] = 0;
        _Model.tmp_priority_depth_sum[priority] = 0;
    }
    for (int depth = clear_depths - 1; depth >= 0; depth--) {
        int face_count = _Model.tmp_depth_face_count[depth];
        int *depth_faces = _Model.tmp_depth_faces[depth];
        for (int n = 0; n < face_count; n++) {
            int face = depth_faces[n];
            int priority = m->face_priorities[face];
            if (priority < 0 || priority >= MODEL_PRIORITY_COUNT) {
#ifdef __PS2__
                ps2_dropped_invalid_faces++;
#endif
                continue;
            }
            int priority_face_count = _Model.tmp_priority_face_count[priority];
            if (priority_face_count >= MODEL_PRIORITY_FACE_COUNT) {
#ifdef __PS2__
                ps2_dropped_priority_faces++;
#endif
                continue;
            }
            _Model.tmp_priority_faces[priority][priority_face_count] = face;
            _Model.tmp_priority_face_count[priority] = priority_face_count + 1;
            if (priority < 10) {
                _Model.tmp_priority_depth_sum[priority] += depth;
            } else if (priority == 10) {
                _Model.tmp_priority10_face_depth[priority_face_count] = depth;
            } else {
                _Model.tmp_priority11_face_depth[priority_face_count] = depth;
            }
        }
    }

    int averagePriorityDepthSum1_2 = 0;
    if (_Model.tmp_priority_face_count[1] > 0 || _Model.tmp_priority_face_count[2] > 0)
        averagePriorityDepthSum1_2 = (_Model.tmp_priority_depth_sum[1] + _Model.tmp_priority_depth_sum[2]) /
                                     (_Model.tmp_priority_face_count[1] + _Model.tmp_priority_face_count[2]);
    int averagePriorityDepthSum3_4 = 0;
    if (_Model.tmp_priority_face_count[3] > 0 || _Model.tmp_priority_face_count[4] > 0)
        averagePriorityDepthSum3_4 = (_Model.tmp_priority_depth_sum[3] + _Model.tmp_priority_depth_sum[4]) /
                                     (_Model.tmp_priority_face_count[3] + _Model.tmp_priority_face_count[4]);
    int averagePriorityDepthSum6_8 = 0;
    if (_Model.tmp_priority_face_count[6] > 0 || _Model.tmp_priority_face_count[8] > 0)
        averagePriorityDepthSum6_8 = (_Model.tmp_priority_depth_sum[6] + _Model.tmp_priority_depth_sum[8]) /
                                     (_Model.tmp_priority_face_count[6] + _Model.tmp_priority_face_count[8]);

    int priority_face = 0;
    int priority_face_count = _Model.tmp_priority_face_count[10];
    int *faces = _Model.tmp_priority_faces[10];
    int *priorities = _Model.tmp_priority10_face_depth;
    if (priority_face_count == 0) {
        priority_face_count = _Model.tmp_priority_face_count[11];
        faces = _Model.tmp_priority_faces[11];
        priorities = _Model.tmp_priority11_face_depth;
    }
    int priority_depth = priority_face_count > 0 ? priorities[0] : -1000;
    for (int p = 0; p < 10; p++) {
        while (p == 0 && priority_depth > averagePriorityDepthSum1_2) {
            model_draw_face(m, faces[priority_face++]);
            if (priority_face == priority_face_count && faces != _Model.tmp_priority_faces[11]) {
                priority_face = 0;
                priority_face_count = _Model.tmp_priority_face_count[11];
                faces = _Model.tmp_priority_faces[11];
                priorities = _Model.tmp_priority11_face_depth;
            }
            priority_depth = priority_face < priority_face_count ? priorities[priority_face] : -1000;
        }
        while (p == 3 && priority_depth > averagePriorityDepthSum3_4) {
            model_draw_face(m, faces[priority_face++]);
            if (priority_face == priority_face_count && faces != _Model.tmp_priority_faces[11]) {
                priority_face = 0;
                priority_face_count = _Model.tmp_priority_face_count[11];
                faces = _Model.tmp_priority_faces[11];
                priorities = _Model.tmp_priority11_face_depth;
            }
            priority_depth = priority_face < priority_face_count ? priorities[priority_face] : -1000;
        }
        while (p == 5 && priority_depth > averagePriorityDepthSum6_8) {
            model_draw_face(m, faces[priority_face++]);
            if (priority_face == priority_face_count && faces != _Model.tmp_priority_faces[11]) {
                priority_face = 0;
                priority_face_count = _Model.tmp_priority_face_count[11];
                faces = _Model.tmp_priority_faces[11];
                priorities = _Model.tmp_priority11_face_depth;
            }
            priority_depth = priority_face < priority_face_count ? priorities[priority_face] : -1000;
        }
        int n = _Model.tmp_priority_face_count[p];
        int *tris = _Model.tmp_priority_faces[p];
        for (int f = 0; f < n; f++) model_draw_face(m, tris[f]);
    }
    while (priority_depth != -1000) {
        model_draw_face(m, faces[priority_face++]);
        if (priority_face == priority_face_count && faces != _Model.tmp_priority_faces[11]) {
            priority_face = 0;
            faces = _Model.tmp_priority_faces[11];
            priority_face_count = _Model.tmp_priority_face_count[11];
            priorities = _Model.tmp_priority11_face_depth;
        }
        priority_depth = priority_face < priority_face_count ? priorities[priority_face] : -1000;
    }
}