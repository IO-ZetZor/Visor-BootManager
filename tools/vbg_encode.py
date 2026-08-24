"""
vbg_encode.py - encode a video as a Visor BackGround (.vbg) wallpaper.

VBG is a keyframe + tiled-quantized-delta container with motion compensation:
  header: "VISORVBG" u32le version=3 u32le width u32le height u32le nframes
          u32le delay_ms u32le flags u32le max_delta      (40 bytes total)
          flags bit 0 = stream uses motion compensation
          max_delta   = largest inflated delta payload, so the decoder sizes
                        its scratch to actual need, not the worst case
  frames: u8 type u32le size payload

  type 0  JPEG keyframe.
  type 1  flat delta (v1, still decoded): DEFLATE of w*h*3 signed bytes.
  type 2  tiled quantized delta:
            u8 qshift, u8 tile_log2, u16le ntx, u16le nty,
            u8 bitmap[(ntx*nty+7)//8]   bit set = tile coded,
            DEFLATE of the coded tiles in raster order, each a full
            (1<<tile_log2)^2 * 3 block of signed quantized values.
  type 3  as type 2, but the inflated stream begins with one
          (i8 mvy, i8 mvx) pair per coded tile.  The prediction is copied
          from the previous frame at (y+mvy, x+mvx) before the residual is
          added, which turns translating content into near-zero residual.

  The decoder reconstructs px += q << qshift.  Edge tiles are zero-padded
  to a constant stride so both sides step by a fixed amount; the decoder
  applies only the clipped region.

Knobs: --threshold is a per-channel dead zone, --qshift sets the residual
quantizer (Q = 1 << qshift), --mv-range bounds the motion search (0 off).
--denoise runs a temporal filter first; per-pixel grain otherwise dirties
every tile and collapses the stream back to keyframes.

Usage: vbg_encode.py INPUT OUTPUT [--scale WxH|H] [--quality N]
                     [--threshold T] [--qshift S] [--tile N] [--keyint N]
                     [--mv-range N] [--denoise SPEC|none] [--fps F]
"""

import functools
import itertools
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from concurrent.futures import ThreadPoolExecutor

try:
    import numpy as np
except ImportError:
    sys.stderr.write("vbg_encode: numpy is required (pip install numpy)\n")
    sys.exit(1)

def need(prog):
    if not shutil.which(prog):
        sys.stderr.write("vbg_encode: %s is required\n" % prog)
        sys.exit(1)

def ffprobe(path):
    """Source stream properties.  ffprobe cannot apply -vf, so post-filter
    dimensions come from filtered_dims() instead."""
    cmd = ["ffprobe", "-v", "error", "-select_streams", "v:0",
           "-show_entries",
           "stream=width,height,r_frame_rate,avg_frame_rate,nb_frames",
           "-of", "default=noprint_wrappers=1", "-i", path]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    info = {}
    for line in out.splitlines():
        k, _, v = line.partition("=")
        if k and v:
            info[k.strip()] = v.strip()
    if "width" not in info or "height" not in info:
        sys.stderr.write("vbg_encode: no video stream in %s\n" % path)
        sys.exit(1)
    return info

def filtered_dims(path, vf, fallback):
    """Dimensions after the filter chain, read back from showinfo."""
    if not vf:
        return fallback
    chain = vf[1] + ",showinfo"
    r = subprocess.run(
        ["ffmpeg", "-v", "info", "-i", path, "-vf", chain,
         "-frames:v", "1", "-f", "null", "-"],
        capture_output=True, text=True)
    m = None
    for line in r.stderr.splitlines():
        hit = re.search(r"\bs:(\d+)x(\d+)\b", line)
        if hit:
            m = hit
            break
    if not m:
        sys.stderr.write("vbg_encode: could not determine filtered size "
                         "for %s\n" % path)
        sys.exit(1)
    return int(m.group(1)), int(m.group(2))

def _fps_str(f):
    """Render a frame rate for an ffmpeg filter without float noise."""
    return str(int(f)) if float(f).is_integer() else repr(float(f))

def parse_rate(s):
    try:
        if "/" in s:
            n, d = s.split("/")
            return float(n) / float(d) if float(d) else 0.0
        return float(s)
    except (ValueError, ZeroDivisionError):
        return 0.0

def _raw_stream(cmd, w, h):
    """Yield (h, w, 3) int16 frames from an ffmpeg rawvideo pipe."""
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    size = w * h * 3
    try:
        while True:
            buf = bytearray()
            while len(buf) < size:
                chunk = p.stdout.read(size - len(buf))
                if not chunk:
                    break
                buf += chunk
            if len(buf) < size:
                break
            yield (np.frombuffer(bytes(buf), dtype=np.uint8)
                     .reshape(h, w, 3).astype(np.int16))
    finally:
        p.stdout.close()
        p.wait()

def source_frames(path, w, h, vf):
    """Source frames, streamed: a long clip must not be held in memory."""
    return _raw_stream(
        ["ffmpeg", "-v", "error", "-i", path] + (vf or []) +
        ["-f", "rawvideo", "-pix_fmt", "rgb24", "-"], w, h)

def jpeg_set(path, w, h, vf, quality, tmpdir):
    """Encode every frame to an independent JPEG, then stream them back.

    Returns (list of JPEG paths, iterator of (h,w,3) int16 reconstructions).
    Two ffmpeg passes total rather than three per frame, and the decoded
    reconstructions are streamed so a 4K clip does not need gigabytes.
    """
    pat = os.path.join(tmpdir, "f%06d.jpg")
    subprocess.run(
        ["ffmpeg", "-v", "error", "-i", path] + (vf or []) +
        ["-c:v", "mjpeg", "-q:v", str(quality), pat],
        check=True)

    names = sorted(n for n in os.listdir(tmpdir) if n.endswith(".jpg"))
    paths = [os.path.join(tmpdir, n) for n in names]
    recons = _raw_stream(
        ["ffmpeg", "-v", "error", "-i", pat,
         "-f", "rawvideo", "-pix_fmt", "rgb24", "-"], w, h)
    return paths, recons

def _quantize(d, thresh, qshift):
    """Dead zone plus round-to-nearest quantization, clamped to int8 range.

    Folds the dead zone in and works on |d| so this is a few in-place passes
    over the frame instead of np.where evaluating both branches of a signed
    shift.  Zeroed entries survive the rounding add because (q >> 1) >> qshift
    is 0, so the dead zone still lands exactly on zero.
    """
    q = 1 << qshift
    a = np.abs(d)
    if thresh > 0:
        a = np.where(a <= thresh, np.int16(0), a)
    a += q >> 1
    a >>= qshift
    np.clip(a, 0, 127, out=a)
    return np.where(d < 0, -a, a)

def _pad_to_tiles(a, nty, ntx, t):
    """Zero-pad to whole tiles, keeping the plain (ph, pw, 3) layout."""
    h, w, c = a.shape
    pad = np.zeros((nty * t, ntx * t, c), dtype=a.dtype)
    pad[:h, :w, :] = a
    return pad

def _tile_validity(h, w, nty, ntx, t):
    """Per-axis bounds for each tile's clipped extent, reused per candidate."""
    y0 = np.arange(nty, dtype=np.int32) * t
    x0 = np.arange(ntx, dtype=np.int32) * t
    return y0, x0, np.minimum(t, h - y0), np.minimum(t, w - x0)

def _sad_per_tile(curp, prevp, nty, ntx, t, dy, dx, buf):
    """Sum |curp - prevp shifted by (dy, dx)| within each tile.

    Works on the padded buffers so the per-tile reduction is one reshape and
    one sum.  buf is reused across candidates to keep the search allocation
    free, which matters because this runs once per candidate vector.
    """
    ph, pw, nc = curp.shape
    buf[:] = 0
    ys, ye = max(0, -dy), min(ph, ph - dy)
    xs, xe = max(0, -dx), min(pw, pw - dx)
    if ye > ys and xe > xs:
        np.subtract(curp[ys:ye, xs:xe], prevp[ys + dy:ye + dy, xs + dx:xe + dx],
                    out=buf[ys:ye, xs:xe])
        np.abs(buf[ys:ye, xs:xe], out=buf[ys:ye, xs:xe])
    return buf.reshape(nty, t, ntx, t, nc).sum(axis=(1, 3, 4), dtype=np.int64)

def _quarters(a):
    """The four 2x2 phase sub-planes of a, dropping any odd row/column.

    Returned as views so a caller can accumulate them into one widened buffer
    a quarter of the frame's size, rather than promoting the whole frame.
    """
    h, w, c = a.shape
    h2, w2 = h // 2, w // 2
    if h2 == 0 or w2 == 0:
        return None
    v = a[:h2 * 2, :w2 * 2, :].reshape(h2, 2, w2, 2, c)
    return v[:, 0, :, 0, :], v[:, 0, :, 1, :], v[:, 1, :, 0, :], v[:, 1, :, 1, :]

_YR, _YG, _YB = 77, 150, 29

def _luma(a):
    """Rec.601 luma as a contiguous (h, w, 1) int16 plane."""
    y = a[:, :, 1].astype(np.int32) * _YG
    y += a[:, :, 0] * _YR
    y += a[:, :, 2] * _YB
    y >>= 8
    return np.ascontiguousarray(y.astype(np.int16))[:, :, None]

def _luma_half(a):
    """Half-resolution luma straight from RGB, as a (h/2, w/2, 1) int16 plane.

    The coarse search only ever looks at the halved plane, so going through a
    full-resolution luma first would allocate and walk four times the memory
    for nothing.  Both operations are linear, so averaging then weighting
    matches weighting then averaging up to the shared rounding shift.
    """
    q = _quarters(a)
    if q is None:
        return None
    s = q[0].astype(np.int32)
    for part in q[1:]:
        s += part
    y = s[:, :, 0] * _YR
    y += s[:, :, 1] * _YG
    y += s[:, :, 2] * _YB
    y >>= 10
    return np.ascontiguousarray(y.astype(np.int16))[:, :, None]

WORKERS = max(1, min(16, os.cpu_count() or 1))

@functools.lru_cache(maxsize=None)
def _pool():
    """Thread pool built once and reused for every frame's SAD sweeps.

    numpy releases the GIL inside the subtract/abs/sum that dominate a
    candidate pass, so plain threads scale nearly linearly here and cost
    none of multiprocessing's per-frame pickling of 4K planes.
    """
    return ThreadPoolExecutor(WORKERS) if WORKERS > 1 else None

def _scan_chunk(curp, prevp, nty, ntx, t, cands, bounds, h, w):
    """SAD-minimize over one contiguous slice of the candidate list.

    Each chunk keeps its own scratch and its own running best, so workers
    share nothing; the caller merges them back in candidate order.
    """
    y0, x0, th, tw = bounds
    buf = np.empty_like(curp)
    best = np.full((nty, ntx), np.iinfo(np.int64).max, dtype=np.int64)
    mvy = np.zeros((nty, ntx), dtype=np.int16)
    mvx = np.zeros((nty, ntx), dtype=np.int16)
    for dy, dx in cands:
        oky = (y0 + dy >= 0) & (y0 + dy + th <= h)
        okx = (x0 + dx >= 0) & (x0 + dx + tw <= w)
        if not (oky.any() and okx.any()):
            continue
        sad = _sad_per_tile(curp, prevp, nty, ntx, t, dy, dx, buf)
        better = (oky[:, None] & okx[None, :]) & (sad < best)
        if better.any():
            best = np.where(better, sad, best)
            mvy = np.where(better, np.int16(dy), mvy)
            mvx = np.where(better, np.int16(dx), mvx)
    return best, mvy, mvx

def _best_of_candidates(cur, prev, nty, ntx, t, cands):
    """Minimize per-tile SAD over an explicit candidate vector list.

    The candidate list is split across threads in contiguous slices and the
    per-slice winners merged in list order under a strict <, which reproduces
    the serial "first candidate to reach the minimum wins" tie-break exactly,
    so the result does not depend on how the threads interleave.
    """
    h, w, _ = cur.shape
    curp = _pad_to_tiles(cur, nty, ntx, t)
    prevp = _pad_to_tiles(prev, nty, ntx, t)
    bounds = _tile_validity(h, w, nty, ntx, t)

    ex = _pool()
    nw = 1 if ex is None else min(WORKERS, len(cands))
    if nw <= 1:
        parts = [_scan_chunk(curp, prevp, nty, ntx, t, cands, bounds, h, w)]
    else:
        step = -(-len(cands) // nw)
        slices = [cands[i:i + step] for i in range(0, len(cands), step)]
        parts = list(ex.map(
            lambda cs: _scan_chunk(curp, prevp, nty, ntx, t, cs, bounds, h, w),
            slices))

    best, mvy, mvx = parts[0]
    for cbest, cmvy, cmvx in parts[1:]:
        better = cbest < best
        if better.any():
            best = np.where(better, cbest, best)
            mvy = np.where(better, cmvy, mvy)
            mvx = np.where(better, cmvx, mvx)
    return best, mvy, mvx

REFINE = 1
MIN_COARSE_TILE = 2
MV_TILE_COST = 4

def motion_search(cur, prev, tlog, mv_range):
    """Per-tile integer motion search, hierarchical and vectorized over tiles.

    An exhaustive search costs O(range^2) full-frame passes, impractical at 4K
    beyond a tiny range.  Instead sweep the whole range on a half-resolution
    luma level -- a quarter of the pixels and a quarter of the candidates for
    the same reach -- then refine per tile at full resolution.  The two halves
    are cheap for opposite reasons: the sweep because it is reduced, the
    refinement because it only touches tiles that moved.

    Halving once is as far as this pays: a quarter-scale level would shrink a
    16px tile to 4px of luma, too little signal to match on, and it measured
    ~9% worse compression for no reliable time saving.

    The sweep compares luma; the refinement compares RGB, which by then costs
    little because it is sparse.
    """
    h, w, _ = cur.shape
    t = 1 << tlog
    ntx, nty = (w + t - 1) // t, (h + t - 1) // t

    cur2, prev2 = _luma_half(cur), _luma_half(prev)
    coarse_ok = (cur2 is not None and prev2 is not None and
                 t // 2 >= MIN_COARSE_TILE and mv_range >= 2)
    if coarse_ok:
        t2 = t // 2
        n2x, n2y = (cur2.shape[1] + t2 - 1) // t2, (cur2.shape[0] + t2 - 1) // t2

        coarse_ok = (n2x, n2y) == (ntx, nty)

    if not coarse_ok:

        full = [(dy, dx) for dy in range(-mv_range, mv_range + 1)
                for dx in range(-mv_range, mv_range + 1)]
        _, mvy, mvx = _best_of_candidates(_luma(cur), _luma(prev), nty, ntx, t,
                                          full)
        return mvy, mvx

    r2 = mv_range // 2
    cands = [(dy, dx) for dy in range(-r2, r2 + 1)
             for dx in range(-r2, r2 + 1)]
    _, cy, cx = _best_of_candidates(cur2, prev2, nty, ntx, t // 2, cands)

    return _refine_sparse(cur, prev, (cy * 2).astype(np.int16),
                          (cx * 2).astype(np.int16), t, REFINE, mv_range)

def _to_grid(a, nty, ntx, t):
    """Zero-pad to whole tiles and view as (nty, ntx, t, t, 3)."""
    h, w, c = a.shape
    pad = np.zeros((nty * t, ntx * t, c), dtype=a.dtype)
    pad[:h, :w, :] = a
    return pad.reshape(nty, t, ntx, t, c).transpose(0, 2, 1, 3, 4)

def _from_grid(g, h, w, t):
    nty, ntx = g.shape[0], g.shape[1]
    return g.transpose(0, 2, 1, 3, 4).reshape(nty * t, ntx * t, -1)[:h, :w, :]

def _gather_tiles(src, ty, tx, vy, vx, t):
    """Tiles of src at grid (ty, tx), displaced by per-tile (vy, vx).

    Returns (n, t, t, c).  Positions past the frame edge are zeroed to match
    the zero padding _to_grid() gives cur, so an edge tile's padded region
    contributes no residual.
    """
    h, w, _ = src.shape
    ar = np.arange(t, dtype=np.int32)
    dy = (ty.astype(np.int32) * t)[:, None] + ar
    dx = (tx.astype(np.int32) * t)[:, None] + ar
    sy = dy + vy.astype(np.int32)[:, None]
    sx = dx + vx.astype(np.int32)[:, None]

    gath = src[np.clip(sy, 0, h - 1)[:, :, None],
               np.clip(sx, 0, w - 1)[:, None, :], :]
    keep = ((dy < h)[:, :, None] & (dx < w)[:, None, :])[..., None]
    return np.where(keep, gath, np.int16(0)).astype(np.int16)

def _tiles_valid(ty, tx, vy, vx, h, w, t):
    """Per-tile mask of vectors whose source stays inside the frame.

    The decoder refuses an entire frame that contains a vector reaching
    outside the picture, so this bound is a hard constraint, not a heuristic.
    """
    y0 = ty.astype(np.int32) * t
    x0 = tx.astype(np.int32) * t
    th = np.minimum(t, h - y0)
    tw = np.minimum(t, w - x0)
    sy, sx = y0 + vy, x0 + vx
    return (sy >= 0) & (sy + th <= h) & (sx >= 0) & (sx + tw <= w)

def _gather_patch(src, ty, tx, vy, vx, t, margin):
    """(n, t+2m, t+2m, c) window of src around each tile's displaced position.

    Edge-clamped rather than zero-padded, because this feeds the search metric
    only: a tile overhanging the frame is merely scored slightly differently,
    never reconstructed from clamped pixels.  One patch per tile lets a whole
    search window be sliced out of it instead of re-gathering per offset.
    """
    h, w, _ = src.shape
    ar = np.arange(t + 2 * margin, dtype=np.int32) - margin
    sy = (ty.astype(np.int32) * t + vy.astype(np.int32))[:, None] + ar
    sx = (tx.astype(np.int32) * t + vx.astype(np.int32))[:, None] + ar
    return src[np.clip(sy, 0, h - 1)[:, :, None],
               np.clip(sx, 0, w - 1)[:, None, :], :]

def _refine_sparse(cur, prev, mvy, mvx, t, radius, mv_range):
    """Per-tile full-resolution refinement of the coarse vectors.

    Touches only the tiles that actually moved, so the cost tracks the
    moved-tile count rather than the frame area.  That is what makes a genuine
    per-tile search affordable here: refining by testing the union of every
    tile's window as full-frame shifts needs a frame pass per distinct vector,
    which on incoherent motion runs to a thousand passes -- so it had to be
    abandoned wholesale past a cap, losing the refinement exactly when the
    motion was interesting.  At this sparsity the metric can afford full RGB.
    """
    ty, tx = np.nonzero((mvy != 0) | (mvx != 0))
    if not len(ty):
        return mvy, mvx
    h, w, _ = cur.shape
    z = np.zeros(len(ty), dtype=np.int16)
    base = _gather_patch(cur, ty, tx, z, z, t, 0)
    cy0, cx0 = mvy[ty, tx], mvx[ty, tx]
    patch = _gather_patch(prev, ty, tx, cy0, cx0, t, radius)

    def sad(pred):
        d = base - pred
        np.abs(d, out=d)
        return d.sum(axis=(1, 2, 3), dtype=np.int64)

    best, vy, vx = sad(_gather_patch(prev, ty, tx, z, z, t, 0)), z, z
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            ny, nx = (cy0 + dy).astype(np.int16), (cx0 + dx).astype(np.int16)

            ok = ((np.abs(ny) <= mv_range) & (np.abs(nx) <= mv_range) &
                  _tiles_valid(ty, tx, ny, nx, h, w, t))
            if not ok.any():
                continue
            i, j = dy + radius, dx + radius
            s = sad(patch[:, i:i + t, j:j + t, :])
            take = ok & (s < best)
            if take.any():
                best = np.where(take, s, best)
                vy = np.where(take, ny, vy)
                vx = np.where(take, nx, vx)
    out_y, out_x = np.zeros_like(mvy), np.zeros_like(mvx)
    out_y[ty, tx] = vy
    out_x[ty, tx] = vx
    return out_y, out_x

def _build_payload(gprev, gpred, qsel, mvy, mvx, with_mv, prev, h, w, t, tlog,
                   qshift, nty, ntx):
    """Assemble one delta candidate.

    Returns (payload, frame_type, ncoded, inflated_bytes, make_recon).  The
    reconstruction sits behind a thunk because only the winning candidate's
    canvas is ever used and building it costs a full frame pass.
    """
    q = 1 << qshift
    coded = np.any(qsel != 0, axis=(2, 3, 4))
    if with_mv:
        coded = coded | (mvy != 0) | (mvx != 0)
    ncoded = int(coded.sum())
    bitmap = np.packbits(coded.reshape(-1), bitorder="little").tobytes()
    header = struct.pack("<BBHH", qshift, tlog, ntx, nty)

    if ncoded == 0:
        co = zlib.compressobj(9, zlib.DEFLATED, -15)
        return (header + bitmap + co.compress(b"") + co.flush(),
                2, 0, 0, lambda: prev.copy())

    body = b""
    if with_mv:
        body += np.stack([mvy[coded], mvx[coded]],
                         axis=-1).astype(np.int8).tobytes()
    body += np.ascontiguousarray(qsel[coded]).astype(np.int8).tobytes()

    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    payload = header + bitmap + co.compress(body) + co.flush()

    def make_recon():
        out = gprev.copy()
        out[coded] = np.clip(gpred[coded] + qsel[coded].astype(np.int32) * q,
                             0, 255).astype(np.int16)
        return _from_grid(out, h, w, t).astype(np.int16)

    return payload, (3 if with_mv else 2), ncoded, len(body), make_recon

def tiled_delta(cur, prev, thresh, qshift, tlog, mv_range=0):
    """Build a type-2 or type-3 payload plus the canvas the decoder will hold.

    Returns (payload, frame_type, reconstruction, ncoded, inflated_bytes).
    A tile is coded when it carries a nonzero vector or a nonzero residual;
    everything else costs a single bitmap bit.  Both candidates are assembled
    and the smaller wins, so enabling motion compensation can never make a
    frame bigger -- a per-tile coefficient count alone does not account for
    the two vector bytes each coded tile then has to carry.
    """
    h, w, _ = cur.shape
    t = 1 << tlog
    ntx, nty = (w + t - 1) // t, (h + t - 1) // t

    gcur = _to_grid(cur, nty, ntx, t)
    gprev = _to_grid(prev, nty, ntx, t)
    zero = np.zeros((nty, ntx), dtype=np.int16)

    q0 = _quantize(gcur - gprev, thresh, qshift)
    cand = [_build_payload(gprev, gprev, q0, zero, zero, False,
                           prev, h, w, t, tlog, qshift, nty, ntx)]

    if mv_range > 0:
        mvy, mvx = motion_search(cur, prev, tlog, mv_range)

        ty, tx = np.nonzero((mvy != 0) | (mvx != 0))
        if len(ty):
            pred = _gather_tiles(prev, ty, tx, mvy[ty, tx], mvx[ty, tx], t)
            q1 = _quantize(gcur[ty, tx] - pred, thresh, qshift)

            nz0 = np.count_nonzero(q0[ty, tx], axis=(1, 2, 3))
            nz1 = np.count_nonzero(q1, axis=(1, 2, 3))
            take = (nz1 + MV_TILE_COST) < nz0
            if take.any():
                sy, sx = ty[take], tx[take]
                qsel = q0.copy()
                qsel[sy, sx] = q1[take]

                gpred = gprev.copy()
                gpred[sy, sx] = pred[take]
                smvy, smvx = np.zeros_like(mvy), np.zeros_like(mvx)
                smvy[sy, sx] = mvy[sy, sx]
                smvx[sy, sx] = mvx[sy, sx]
                cand.append(_build_payload(gprev, gpred, qsel, smvy, smvx,
                                           True, prev, h, w, t, tlog, qshift,
                                           nty, ntx))

    payload, ptype, ncoded, inflated, make_recon = min(
        cand, key=lambda c: len(c[0]))
    return payload, ptype, make_recon(), ncoded, inflated

def main():
    args = sys.argv[1:]
    if len(args) < 2 or args[0] in ("-h", "--help"):
        sys.stderr.write(
            "usage: vbg_encode.py INPUT OUTPUT [--scale WxH|H] [--quality N]\n"
            "       [--threshold T] [--qshift S] [--tile N] [--keyint N]\n"
            "       [--mv-range N] [--denoise SPEC|none] [--fps F]\n")
        sys.exit(1)

    src, dst = args[0], args[1]
    scale = None
    quality = 3
    thresh = 6
    qshift = 3
    tile = 16
    keyint = 60
    mv_range = 8
    denoise = "hqdn3d=4:3:6:4"
    progress = 0
    auto_tune = True
    fps = None

    i = 2
    while i < len(args):
        a = args[i]
        if a == "--progress":
            progress = 10
            i += 1
            continue
        if a in ("--scale", "--quality", "--threshold", "--qshift",
                 "--tile", "--keyint", "--mv-range", "--denoise",
                 "--fps") and i + 1 < len(args):
            val = args[i + 1]
            if a == "--scale":
                scale = val
            elif a == "--quality":
                quality = int(val)
            elif a == "--threshold":
                thresh = int(val)
                auto_tune = False
            elif a == "--qshift":
                qshift = int(val)
                auto_tune = False
            elif a == "--tile":
                tile = int(val)
            elif a == "--keyint":
                keyint = int(val)
            elif a == "--mv-range":
                mv_range = int(val)
            elif a == "--denoise":
                denoise = None if val == "none" else val
            else:
                fps = float(val)
            i += 2
        else:
            sys.stderr.write("vbg_encode: unknown option %s\n" % a)
            sys.exit(1)

    if not 0 <= qshift <= 7:
        sys.stderr.write("vbg_encode: --qshift must be 0..7\n")
        sys.exit(1)
    if not 0 <= mv_range <= 127:
        sys.stderr.write("vbg_encode: --mv-range must be 0..127 (0 disables)\n")
        sys.exit(1)
    tlog = tile.bit_length() - 1
    if tile != (1 << tlog) or not 2 <= tlog <= 6:
        sys.stderr.write("vbg_encode: --tile must be a power of two, 4..64\n")
        sys.exit(1)
    if keyint < 1:
        sys.stderr.write("vbg_encode: --keyint must be >= 1\n")
        sys.exit(1)

    need("ffmpeg")
    need("ffprobe")

    vf_parts = []

    if fps is not None:
        if fps <= 0:
            sys.stderr.write("vbg_encode: --fps must be positive\n")
            sys.exit(1)
        vf_parts.append("fps=%s" % _fps_str(fps))
    if scale:
        if "x" in scale.lower():
            sw, _, sh = scale.lower().partition("x")
            vf_parts.append("scale=%s:%s:force_original_aspect_ratio=decrease"
                            ":flags=lanczos" % (sw, sh))
        else:
            vf_parts.append("scale=-2:%s:force_original_aspect_ratio=decrease"
                            ":flags=lanczos" % scale)

    if denoise:
        vf_parts.append(denoise)
    vf = ["-vf", ",".join(vf_parts)] if vf_parts else None

    info = ffprobe(src)
    w, h = filtered_dims(src, vf, (int(info["width"]), int(info["height"])))
    if fps is None:
        fps = parse_rate(info.get("r_frame_rate") or
                         info.get("avg_frame_rate") or "")
    if fps <= 0:
        fps = 30.0
    delay = max(1, min(60000, int(round(1000.0 / fps))))

    tmpdir = tempfile.mkdtemp(prefix="vbg")
    try:
        srcs = source_frames(src, w, h, vf)
        jpaths, recons = jpeg_set(src, w, h, vf, quality, tmpdir)
        if not jpaths:
            sys.stderr.write("vbg_encode: decoded no frames from %s\n" % src)
            sys.exit(1)

        if auto_tune:
            s0 = next(srcs, None)
            r0 = next(recons, None)
            if s0 is None or r0 is None:
                sys.stderr.write("vbg_encode: decoded no frames from %s\n" % src)
                sys.exit(1)
            floor = float(np.percentile(np.abs(s0 - r0), 99.0))
            thresh = int(min(48, max(8, round(floor * 2.0))))
            qshift = 5 if thresh >= 32 else (4 if thresh >= 16 else 3)
            sys.stdout.write(
                "vbg: keyframe noise floor p99=%.1f -> threshold=%d qshift=%d "
                "(override with --threshold/--qshift)\n"
                % (floor, thresh, qshift))
            srcs = itertools.chain([s0], srcs)
            recons = itertools.chain([r0], recons)

        out = open(dst, "wb")
        out.write(b"VISORVBG")
        out.write(b"\x00" * 32)

        written = 0
        nkey = 0
        keybytes = 0
        deltabytes = 0
        tiles_sent = 0
        tiles_total = 0
        n_mc = 0
        mc_tiles = 0
        max_delta = 0
        used_mc = False
        prev = None
        ntx = (w + (1 << tlog) - 1) >> tlog
        nty = (h + (1 << tlog) - 1) >> tlog

        for idx, (cur, recon) in enumerate(zip(srcs, recons)):
            if idx >= len(jpaths):
                break
            jpeg = open(jpaths[idx], "rb").read()
            force_key = prev is None or idx % keyint == 0

            payload, ptype = None, 0
            if not force_key:
                dpay, dtype, drecon, nch, inflated = tiled_delta(
                    cur, prev, thresh, qshift, tlog, mv_range)

                if len(dpay) < len(jpeg):
                    payload, ptype = dpay, dtype
                    prev = drecon
                    tiles_sent += nch
                    tiles_total += ntx * nty
                    deltabytes += len(dpay)
                    max_delta = max(max_delta, inflated)
                    if dtype == 3:
                        used_mc = True
                        n_mc += 1
                        mc_tiles += nch

            if payload is None:
                payload, ptype = jpeg, 0
                prev = recon
                nkey += 1
                keybytes += len(payload)

            out.write(struct.pack("<BI", ptype, len(payload)))
            out.write(payload)
            written += 1
            if progress and written % progress == 0:
                sys.stderr.write(
                    "  %d frames, %.1f KB so far\r"
                    % (written, (keybytes + deltabytes) / 1024.0))
                sys.stderr.flush()
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    if progress:
        sys.stderr.write("\n")

    flags = 1 if used_mc else 0
    out.seek(8)
    out.write(struct.pack("<IIIIIII", 3, w, h, written, delay,
                          flags, max_delta))
    out.close()

    total = keybytes + deltabytes
    sys.stdout.write("vbg: %dx%d, %d frames, %d ms, v3%s\n"
                     % (w, h, written, delay, " +mc" if used_mc else ""))
    sys.stdout.write(
        "     %d keyframes (%.1f KB), %d deltas (%.1f KB), total %.1f KB\n"
        % (nkey, keybytes / 1024.0, written - nkey, deltabytes / 1024.0,
           total / 1024.0))
    if tiles_total:
        sys.stdout.write(
            "     tiles sent %d/%d (%.1f%%), tile=%d qshift=%d threshold=%d\n"
            % (tiles_sent, tiles_total, 100.0 * tiles_sent / tiles_total,
               1 << tlog, qshift, thresh))
    if used_mc:
        sys.stdout.write("     motion: %d/%d delta frames, %d coded tiles, "
                         "search +/-%d\n"
                         % (n_mc, written - nkey, mc_tiles, mv_range))

    bound = (((w + 63) // 64 * 64) * ((h + 63) // 64 * 64) * 3)
    sys.stdout.write("     scratch %.1f KB (bound %.1f KB, %.1f%%)\n"
                     % (max_delta / 1024.0, bound / 1024.0,
                        100.0 * max_delta / bound if bound else 0.0))

if __name__ == "__main__":
    main()
