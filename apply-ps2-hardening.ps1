$ErrorActionPreference = 'Stop'

function Normalize([string]$s) {
    return $s.Replace("`r`n", "`n")
}

function Replace-Exact([string]$Path, [string]$Old, [string]$New, [string]$Label) {
    $text = Normalize([IO.File]::ReadAllText($Path))
    $oldText = Normalize($Old)
    $newText = Normalize($New)

    if ($text.Contains($newText)) {
        Write-Host "[already] $Label"
        return
    }
    if (-not $text.Contains($oldText)) {
        throw "Could not find expected source for: $Label ($Path). Refusing to make a fuzzy edit."
    }

    $updated = $text.Replace($oldText, $newText)
    [IO.File]::WriteAllText((Resolve-Path $Path), $updated, [Text.UTF8Encoding]::new($false))
    Write-Host "[applied] $Label"
}

Replace-Exact 'src/clientstream.c' @'
#ifdef __PS2__
'@ @'
#ifdef __PS2__
    // Keep the linear receive window inside its physical 5 KB backing buffer.
    if (len < 0 || len > (int)sizeof(stream->buf)) {
        rs2_error("PS2NET invalid buffered length=%d capacity=%u\n", len, (unsigned)sizeof(stream->buf));
        stream->closed = true;
        return 0;
    }

    int needed = len - stream->bufLen;
    int tail = (int)sizeof(stream->buf) - stream->bufPos - stream->bufLen;
    if (tail < needed && stream->bufPos > 0) {
        memmove(stream->buf, stream->buf + stream->bufPos, stream->bufLen);
        stream->bufPos = 0;
        tail = (int)sizeof(stream->buf) - stream->bufLen;
    }
    if (tail <= 0) {
        return 0;
    }
'@ 'PS2 receive-buffer bounds/compaction'

Replace-Exact 'src/clientstream.c' @'
    int want = (int)sizeof(stream->buf) - stream->bufPos - stream->bufLen;
    if (want < len - stream->bufLen) {
        want = len - stream->bufLen;
    }
'@ @'
    int want = tail;
'@ 'PS2 recv tail capacity'

Replace-Exact 'src/world3d.c' @'
    if (!model && !entity) {
        return false;
    }
    for (int tx = tileX; tx < tileX + tileSizeX; tx++) {
'@ @'
    if (!model && !entity) {
        return false;
    }
    if (level < 0 || level >= world3d->maxLevel || tileSizeX <= 0 || tileSizeZ <= 0) {
        return false;
    }
    if (temporary && world3d->temporaryLocCount >= (int)(sizeof(world3d->temporaryLocs) / sizeof(world3d->temporaryLocs[0]))) {
#ifdef __PS2__
        rs2_error("world3d_add_loc2: temporary location capacity exhausted (%d)\n", world3d->temporaryLocCount);
#endif
        return false;
    }
    for (int tx = tileX; tx < tileX + tileSizeX; tx++) {
'@ 'temporary scene-location capacity'

Replace-Exact 'src/world3d.c' '(yawLevel + 1) % 31' '(yawLevel + 1) % 32' '32-way yaw visibility wrap'

Replace-Exact 'src/model.c' @'
            if (m->pick_aabb) {
                _Model.picked_bitsets[_Model.picked_count++] = key;
            } else {
'@ @'
            if (m->pick_aabb) {
                if (_Model.picked_count < 1000) {
                    _Model.picked_bitsets[_Model.picked_count++] = key;
                }
            } else {
'@ 'picked bitset capacity (AABB)'

Replace-Exact 'src/model.c' @'
                if (depth_average < MODEL_MAX_DEPTH) {
                    _Model.tmp_depth_faces[depth_average][_Model.tmp_depth_face_count[depth_average]++] = f;
                }
'@ @'
                if (depth_average >= 0 && depth_average < MODEL_MAX_DEPTH &&
                    _Model.tmp_depth_face_count[depth_average] < MODEL_DEPTH_FACE_COUNT) {
                    _Model.tmp_depth_faces[depth_average][_Model.tmp_depth_face_count[depth_average]++] = f;
                }
'@ 'depth face bucket capacity'

Replace-Exact 'src/model.c' @'
                    _Model.picked_bitsets[_Model.picked_count++] = bitset;
                    hasInput = false;
'@ @'
                    if (_Model.picked_count < 1000) {
                        _Model.picked_bitsets[_Model.picked_count++] = bitset;
                    }
                    hasInput = false;
'@ 'picked bitset capacity (triangle)'

Replace-Exact 'src/model.c' @'
                    if (depth_average < MODEL_MAX_DEPTH) {
                        _Model.tmp_depth_faces[depth_average][_Model.tmp_depth_face_count[depth_average]++] = f;
                    }
'@ @'
                    if (depth_average >= 0 && depth_average < MODEL_MAX_DEPTH &&
                        _Model.tmp_depth_face_count[depth_average] < MODEL_DEPTH_FACE_COUNT) {
                        _Model.tmp_depth_faces[depth_average][_Model.tmp_depth_face_count[depth_average]++] = f;
                    }
'@ 'clipped depth face bucket capacity'

Replace-Exact 'src/model.c' @'
                int priority_depth = faces[i];
                int depth_average = m->face_priorities[priority_depth];
                int priority_face_count = _Model.tmp_priority_face_count[depth_average]++;
                _Model.tmp_priority_faces[depth_average][priority_face_count] = priority_depth;
'@ @'
                int priority_depth = faces[i];
                int depth_average = m->face_priorities[priority_depth];
                if (depth_average < 0 || depth_average >= 12) {
                    continue;
                }
                int priority_face_count = _Model.tmp_priority_face_count[depth_average];
                if (priority_face_count >= 2000) {
                    continue;
                }
                _Model.tmp_priority_face_count[depth_average] = priority_face_count + 1;
                _Model.tmp_priority_faces[depth_average][priority_face_count] = priority_depth;
'@ 'priority face bucket bounds'

Replace-Exact 'src/world.c' @'
int noise(int x, int y) {
    int n = x + y * 57;
    int n1 = n << 13 ^ n;
    int n2 = n1 * (n1 * n1 * 15731 + 789221) + 1376312589 & INT_MAX;
    return n2 >> 19 & 0xff;
}
'@ @'
int noise(int x, int y) {
    // Preserve the original Java client's defined 32-bit wrapping without C signed-overflow UB.
    uint32_t n = (uint32_t)x + (uint32_t)y * 57u;
    uint32_t n1 = (n << 13) ^ n;
    uint32_t n2 = n1 * (n1 * n1 * 15731u + 789221u) + 1376312589u;
    return (int)((n2 & 0x7fffffffu) >> 19) & 0xff;
}
'@ 'noise 32-bit wrap semantics'

Write-Host ''
Write-Host 'PS2 hardening edits applied successfully.'
