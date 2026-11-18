#!/usr/bin/env python3
"""Reference for the layer's passes (fg/hud_*.comp; tools/hud-motion-check.py compares the two): the motion of HUD content that moves over the
screen (world markers, a ship's flight display) between two captures of the game's HUD layer,
and the layer of a generated frame.

What real consecutive captures showed:
  - a marker is thin text and thin lines drawn at fractions of a pixel: no whole-pixel shift maps
    one capture onto the other pixel by pixel. Sums over blocks of 4x4 pixels do compare.
  - a ship's flight display is one large piece (20 x 6 tiles) that moves with the view; with a
    wide field of view its motion grows towards the screen's edge (19 -> 64 pixels across it in a
    fast turn): a smooth field, not one motion.
  - next to these the layer has content that changes without moving (numbers that count), that
    repeats (scales) and that turns (a map).
A piece that is moved in part tears, which is worse than a piece that steps: a piece moves as a
whole or not at all.

Works on the layer's alpha. Tiles of 64x64 pixels, cells of 4x4 and 16x16 pixels.
  maps      per cell of 4 and of 16: the mean of this capture's alpha where it rose ("arrived"),
            of the one before's where it fell ("left"); for cells of 4 also over all pixels.
  own       every tile with enough arrived content searches: level A (cells of 16, arrived
            against left, +-192 x +-128), level B (cells of 4: around A's answer, and "near":
            all against all around not moved), level C (pixels, around both and not moved): the
            tile's cells of 4 that have changed pixels, the sum of each against the sum of the
            4x4 block of the capture before at the cell's place plus the shift.
  around    every tile with changed cells tries the answers of the tiles around it (two around),
            each one pixel around, on its own cells. -> its motion, and whether it fits.
  pieces    tiles with changed content that touch are one piece. A piece is looked at further
            when a quarter of its arrived content is in tiles whose motion fits, and counts when
            the field then leaves little of what "not moved" leaves (see EXPLAINS) and it is not
            a picture that changes where it stands (see is_picture).
  field     the motion of each tile of a piece that counts: a plane through the motions that fit
            around it (two tiles around), so the field is smooth, a tile whose search went wrong
            is put right and one without an answer has one.
  reach     per tile, the (up to two) rough motions of content that passes through it.
  layer_at  the layer at time t.
What layer_at's rules are worth is measured by shapes.py (scenes with a known answer: a marker
and a filled panel, opaque and half transparent, over nothing and over a field that stands
still) and explain.py (how much of the change between two of the game's captures they explain).
"""
import glob, re
import numpy as np


def load(folder, pid, n):
    """The HUD layer (RGBA16F) of dump n of session pid, as "sc-turbo fg-dump" writes it."""
    path = glob.glob(f"{folder}dump-{pid}-{n:03d}-hud-*.bin")[0]
    w, h = (int(v) for v in re.search(r"-hud-(\d+)x(\d+)\.bin$", path).groups())
    return np.fromfile(path, dtype=np.float16).reshape(h, w, 4)

TILE = 64
EPS = 0.04
ACTIVE = 8           # arrived alpha, in sum (pixels' worth), from which a tile searches
RXA, RYA = 12, 8     # level A: range in cells of 16
MA = 1
RB, ZB = 3, 4        # level B
MB = 2
RC = 3               # level C: +- pixels around B's answers
LEAST = 2            # changed cells a tile must have
CONTENT = 8192       # ... and arrived alpha in them, in 1023ths, in sum (8 pixels' worth)
FIT_NUM, FIT_DEN = 3, 10     # fits: unexplained <= 3/10 of the arrived content
ALIAS_NUM, ALIAS_DEN = 3, 2  # a shift that is much smaller is taken when within 3/2 of the best
ALIAS_FAR = 8        # ... "much smaller": by more than this, in |x| + |y|
ALIAS_BAND = 4
FIRST_NUM, FIRST_DEN = 1, 4   # a piece is looked at further when this share of its arrived content fits
# ... and counts when, over its tiles, the field leaves at most this share of what "not moved"
# leaves. In the game's captures pieces that move leave 0.03 to 0.5 of it (the most where they
# move by two to four pixels, which whole pixels explain less well) and pieces whose content
# changes where it is 0.8 to 1. (The share of the content in tiles that fit, which was asked
# for before, is 40 to 65 % for a flight display in a turn with other things over it: around
# any limit, so that it moved in one frame and stood in the next.)
EXPLAINS_NUM, EXPLAINS_DEN = 3, 5
# ... and when it is not a picture (is_picture). The game's map, top left, is one: see-through, it
# turns and scrolls inside its frame and sways with the frame when the view moves. Whole-pixel shifts
# of its edges explain it well enough to count, and moved it comes out doubled and torn. In the
# captures it has 20 to 34 tiles with content on most of their pixels; the panels at the bottom of
# the view have 8 to 12, whatever else moves 4 at most. (Asking also how much of a piece's content
# changed, or how far its motions spread, took in a ship's HUD that moves slowly: only a few pixels
# of it change from frame to frame. It then stepped at the game's rate and fell behind the rest.)
SOLID_NUM, SOLID_DEN = 3, 5      # a tile with content on this share of its pixels is solid
SOLID_TILES = 16                 # a piece with this many of them is a picture, not something that moves
RR = 2               # refine: +- pixels around the field
SMOOTH = 2           # tiles around that the plane goes through
OFF = 3.0            # a motion further than this from the plane is left out of it
WILD = 10.0          # ... and one further than this from the middle one around, from the start
SAME = 6.0           # motions closer than this are one piece's (reach, field look-up)

PAD = 256


def maps(a_n, a_p, f):
    h, w = a_n.shape
    th, tw = (h + TILE - 1) // TILE, (w + TILE - 1) // TILE
    hc, wc = h // f, w // f
    out = []
    for src in (np.where(a_n - a_p > EPS, a_n, 0.0), np.where(a_p - a_n > EPS, a_p, 0.0), a_n, a_p):
        m = np.zeros((th * TILE // f, tw * TILE // f), dtype=np.float32)
        m[:hc, :wc] = src[: hc * f, : wc * f].reshape(hc, f, wc, f).mean(axis=(1, 3))
        out.append(m)
    return out


def near_best(costs, shifts):
    least = costs.min()
    best = None
    for c, (dx, dy) in zip(costs, shifts):
        if c <= least * 1.05 + 1e-4:
            d2 = dx * dx + dy * dy
            if best is None or d2 < best[0]:
                best = (d2, dx, dy)
    return best[1], best[2]


def level(n, p, weight, x0, y0, size, margin, shifts):
    hc, wc = n.shape
    ys = np.arange(y0 - margin, y0 + size + margin)
    xs = np.arange(x0 - margin, x0 + size + margin)
    yy, xx = np.meshgrid(ys, xs, indexing="ij")
    inside = (yy >= 0) & (yy < hc) & (xx >= 0) & (xx < wc)
    cy, cx = np.clip(yy, 0, hc - 1), np.clip(xx, 0, wc - 1)
    win_n = np.where(inside, n[cy, cx], 0.0)
    win_w = np.where(inside, weight[cy, cx], 0.0) if weight is not None else 1.0
    costs = np.empty(len(shifts), dtype=np.float32)
    for k, (dx, dy) in enumerate(shifts):
        sy, sx = yy + dy, xx + dx
        ok = (sy >= 0) & (sy < hc) & (sx >= 0) & (sx < wc)
        win_p = np.where(ok, p[np.clip(sy, 0, hc - 1), np.clip(sx, 0, wc - 1)], 0.0)
        costs[k] = (win_w * np.abs(win_n - win_p)).sum()
    return costs


def quant(a):
    return (np.clip(a, 0.0, 1.0) * 1023.0 + 0.5).astype(np.int64)


def blocks(q):
    """-> b[y + PAD, x + PAD] = sum of q over the 4x4 pixels from (x, y); outside the picture 0."""
    padded = np.pad(q, PAD + 4)
    c = np.cumsum(np.cumsum(padded, axis=0), axis=1)
    c = np.pad(c, ((1, 0), (1, 0)))
    h, w = q.shape
    hh, ww = h + 2 * PAD, w + 2 * PAD
    return c[8 : 8 + hh, 8 : 8 + ww] - c[4 : 4 + hh, 8 : 8 + ww] - c[8 : 8 + hh, 4 : 4 + ww] + c[4 : 4 + hh, 4 : 4 + ww]


class Frame:
    def __init__(self, a_n, a_p):
        self.a_n, self.a_p = a_n, a_p
        self.h, self.w = a_n.shape
        self.th, self.tw = (self.h + TILE - 1) // TILE, (self.w + TILE - 1) // TILE
        self.q_n, self.q_p = quant(a_n), quant(a_p)
        self.b_n, self.b_p = blocks(self.q_n), blocks(self.q_p)
        self.changed = np.abs(a_n - a_p) > EPS
        self.b_arrived = blocks(np.where(self.q_n - self.q_p > int(EPS * 1023), self.q_n, 0))
        self.filled = self.per_tile((a_n > EPS) | (a_p > EPS))  # pixels with content, in either capture

    def per_tile(self, mask):
        """Pixels of `mask` per tile, in the cells of 4x4 pixels that lie wholly in the picture."""
        hh, ww = self.h // 4 * 4, self.w // 4 * 4
        full = np.zeros((self.th * TILE, self.tw * TILE), dtype=np.int64)
        full[:hh, :ww] = mask[:hh, :ww]
        return full.reshape(self.th, TILE, self.tw, TILE).sum(axis=(1, 3))

    def cells(self, tx, ty):
        py0, px0 = ty * TILE, tx * TILE
        block = self.changed[py0 : py0 + TILE, px0 : px0 + TILE]
        hh, ww = block.shape
        hh, ww = hh // 4 * 4, ww // 4 * 4
        any_ = block[:hh, :ww].reshape(hh // 4, 4, ww // 4, 4).any(axis=(1, 3))
        ys, xs = np.nonzero(any_)
        return xs * 4 + px0, ys * 4 + py0

    def cost(self, xs, ys, dx, dy):
        return int(np.abs(self.b_n[ys + PAD, xs + PAD] - self.b_p[ys + dy + PAD, xs + dx + PAD]).sum())

    def content(self, xs, ys):
        return int(self.b_arrived[ys + PAD, xs + PAD].sum())


def fits(cost, content, zero):
    return FIT_DEN * cost <= FIT_NUM * content and 2 * cost <= zero


def pick(costs, cands, content, zero):
    """The best; but a much smaller shift that also fits is taken instead (what repeats matches
    a period further on, what is the same along a line matches anywhere along it, one of several
    markers that look the same matches the next one)."""
    best = min(range(len(cands)), key=lambda k: (costs[k], k))
    size = lambda d: abs(d[0]) + abs(d[1])
    near = [k for k in range(len(cands)) if fits(costs[k], content, zero) and (cands[k][0] or cands[k][1])]
    if near:
        smallest = min(size(cands[k]) for k in near)
        if size(cands[best]) - smallest > ALIAS_FAR:
            band = [k for k in near if size(cands[k]) <= smallest + ALIAS_BAND]
            best = min(band, key=lambda k: (costs[k], k))
    return cands[best][0], cands[best][1], costs[best]


def own(f, log=None):
    """-> own[ty, tx] = (mx, my, has): each tile's answer from its own search."""
    nm16, pm16, _, _ = maps(f.a_n, f.a_p, 16)
    nm4, pm4, nr4, pr4 = maps(f.a_n, f.a_p, 4)
    weight4 = np.maximum(nm4, pm4)
    out = np.zeros((f.th, f.tw, 3), dtype=np.int64)
    shifts_a = [(dx, dy) for dy in range(-RYA, RYA + 1) for dx in range(-RXA, RXA + 1)]
    shifts_near = [(dx, dy) for dy in range(-ZB, ZB + 1) for dx in range(-ZB, ZB + 1)]
    active = 0
    for ty in range(f.th):
        for tx in range(f.tw):
            arrived = nm16[ty * 4 : ty * 4 + 4, tx * 4 : tx * 4 + 4].sum() * 256.0
            if arrived < ACTIVE:
                continue
            active += 1
            ax, ay = near_best(level(nm16, pm16, None, tx * 4, ty * 4, 4, MA, shifts_a), shifts_a)
            shifts_far = [(ax * 4 + dx, ay * 4 + dy) for dy in range(-RB, RB + 1) for dx in range(-RB, RB + 1)]
            fx, fy = near_best(level(nm4, pm4, None, tx * 16, ty * 16, 16, MB, shifts_far), shifts_far)
            nx, ny = near_best(level(nr4, pr4, weight4, tx * 16, ty * 16, 16, MB, shifts_near), shifts_near)
            xs, ys = f.cells(tx, ty)
            if len(xs) < LEAST:
                continue
            cands = [(fx * 4 + dx, fy * 4 + dy) for dy in range(-RC, RC + 1) for dx in range(-RC, RC + 1)]
            cands += [(nx * 4 + dx, ny * 4 + dy) for dy in range(-RC, RC + 1) for dx in range(-RC, RC + 1)]
            cands += [(0, 0)]
            costs = [f.cost(xs, ys, dx, dy) for (dx, dy) in cands]
            content = f.content(xs, ys)
            dx, dy, cost = pick(costs, cands, content, costs[-1])
            if log is not None:
                log.append((tx, ty, (ax * 16, ay * 16), (fx * 4, fy * 4), (nx * 4, ny * 4), (dx, dy), cost, min(costs), content, costs[-1], len(xs)))
            out[ty, tx] = (dx, dy, 1)
    return out, active


NONE, FITS, NOT, LITTLE = 0, 1, 2, 3


def around(f, mine):
    """-> tiles[ty, tx] = (mx, my, state, cost, content, zero)"""
    out = np.zeros((f.th, f.tw, 6), dtype=np.int64)
    for ty in range(f.th):
        for tx in range(f.tw):
            xs, ys = f.cells(tx, ty)
            if len(xs) == 0:
                continue
            content = f.content(xs, ys)
            zero = f.cost(xs, ys, 0, 0)
            if len(xs) < LEAST or content < CONTENT:
                out[ty, tx] = (0, 0, LITTLE, 0, content, zero)
                continue
            cands = [(0, 0)]
            for sy in range(max(ty - 2, 0), min(ty + 3, f.th)):
                for sx in range(max(tx - 2, 0), min(tx + 3, f.tw)):
                    if mine[sy, sx, 2]:
                        for dy in (-1, 0, 1):
                            for dx in (-1, 0, 1):
                                m = (int(mine[sy, sx, 0]) + dx, int(mine[sy, sx, 1]) + dy)
                                if m not in cands:
                                    cands.append(m)
            costs = [f.cost(xs, ys, dx, dy) for (dx, dy) in cands]
            dx, dy, cost = pick(costs, cands, content, zero)
            good = (dx or dy) and fits(cost, content, zero)
            out[ty, tx] = (dx, dy, FITS if good else NOT, cost, content, zero)
    return out


def is_picture(f, label, l):
    """Whether piece l is a picture that changes where it stands, not something that moves over
    the screen: see SOLID."""
    return int((SOLID_DEN * f.filled[label == l] >= SOLID_NUM * TILE * TILE).sum()) >= SOLID_TILES


def pieces(tiles, label=None, f=None):
    """-> label[ty, tx] (-1: none), counts[label] = whether the piece counts: without `label`
    (first round, the tiles as around() gives them) the pieces are made, and one counts by the
    share of its content that fits; with it (the tiles as refine() gives them) by how much of
    what "not moved" leaves the field leaves and, given the Frame `f`, by not being a picture.
    -> also fitting[label], total[label] (content), left[label] = (by the field, by "not moved")"""
    th, tw, _ = tiles.shape
    second = label is not None
    changed = not second
    if changed:
        label = np.full((th, tw), -1, dtype=np.int64)
        for ty in range(th):
            for tx in range(tw):
                if tiles[ty, tx, 2] != NONE:
                    label[ty, tx] = ty * tw + tx
    while changed:  # the least label of the tiles that touch
        changed = False
        for order in (1, -1):
            for ty in range(th)[::order]:
                for tx in range(tw)[::order]:
                    if label[ty, tx] < 0:
                        continue
                    least = label[ty, tx]
                    for sy in range(max(ty - 1, 0), min(ty + 2, th)):
                        for sx in range(max(tx - 1, 0), min(tx + 2, tw)):
                            if label[sy, sx] >= 0:
                                least = min(least, label[sy, sx])
                    if least != label[ty, tx]:
                        label[ty, tx] = least
                        changed = True
    fitting, total, left = {}, {}, {}
    for ty in range(th):
        for tx in range(tw):
            l = label[ty, tx]
            if l < 0 or tiles[ty, tx, 2] == LITTLE:
                continue
            total[l] = total.get(l, 0) + int(tiles[ty, tx, 4])
            by_field, by_none = left.get(l, (0, 0))
            left[l] = (by_field + int(tiles[ty, tx, 3]), by_none + int(tiles[ty, tx, 5]))
            if tiles[ty, tx, 2] == FITS:
                fitting[l] = fitting.get(l, 0) + int(tiles[ty, tx, 4])
    if second:
        counts = {l: fitting.get(l, 0) > 0 and EXPLAINS_DEN * left[l][0] <= EXPLAINS_NUM * left[l][1]
                  and (f is None or not is_picture(f, label, l)) for l in total}
    else:
        counts = {l: fitting.get(l, 0) > 0 and FIRST_DEN * fitting.get(l, 0) >= FIRST_NUM * total[l] for l in total}
    return label, counts, fitting, total, left


def plane(samples):
    """Least squares m = a + b*x + c*y through (x, y, mx, my, weight): -> (ax, bx, cx), (ay, by, cy);
    with two samples, or all in a line, the slope along the line only; with one, its motion."""
    w = np.array([s[4] for s in samples], dtype=np.float64)
    x = np.array([s[0] for s in samples], dtype=np.float64)
    y = np.array([s[1] for s in samples], dtype=np.float64)
    out = []
    sw = w.sum()
    mx_, my_ = (w * x).sum() / sw, (w * y).sum() / sw
    sxx, syy, sxy = (w * (x - mx_) ** 2).sum(), (w * (y - my_) ** 2).sum(), (w * (x - mx_) * (y - my_)).sum()
    det = sxx * syy - sxy * sxy
    for k in (2, 3):
        v = np.array([s[k] for s in samples], dtype=np.float64)
        mean = (w * v).sum() / sw
        sxv, syv = (w * (x - mx_) * (v - mean)).sum(), (w * (y - my_) * (v - mean)).sum()
        if len(samples) >= 3 and det > 1e-3 * max(sxx, syy, 1e-9) ** 2:
            b = (sxv * syy - syv * sxy) / det
            c = (syv * sxx - sxv * sxy) / det
        elif sxx + syy > 1e-9 and len(samples) >= 2:
            # in a line: the slope along it, none across it (whichever way the line goes; taking
            # the slope across or down by the greater spread is a toss-up for a diagonal)
            b, c = sxv / (sxx + syy), syv / (sxx + syy)
        else:
            b, c = 0.0, 0.0
        out.append((mean - b * mx_ - c * my_, b, c))
    return out


def field(tiles, label, counts):
    """-> motion[ty, tx] = (mx, my, valid) for the tiles of pieces that count."""
    th, tw, _ = tiles.shape
    motion = np.zeros((th, tw, 3), dtype=np.float32)
    for ty in range(th):
        for tx in range(tw):
            l = label[ty, tx]
            if l < 0 or not counts.get(l, False):
                continue
            samples = []
            for sy in range(max(ty - SMOOTH, 0), min(ty + SMOOTH + 1, th)):
                for sx in range(max(tx - SMOOTH, 0), min(tx + SMOOTH + 1, tw)):
                    if label[sy, sx] == l and tiles[sy, sx, 2] == FITS:
                        samples.append((sx - tx, sy - ty, float(tiles[sy, sx, 0]), float(tiles[sy, sx, 1]), float(tiles[sy, sx, 4])))
            if not samples:
                continue
            # the middle motion around (by content), and what is near it: a search that went to
            # the wrong place (a scale's next period, another marker) is far off
            mid = []
            for k in (2, 3):
                order = sorted(samples, key=lambda s: s[k])
                half, run = sum(s[4] for s in order) / 2.0, 0.0
                for s in order:
                    run += s[4]
                    if run >= half:
                        mid.append(s[k])
                        break
            samples = [s for s in samples if abs(s[2] - mid[0]) <= WILD and abs(s[3] - mid[1]) <= WILD]
            if not samples:  # (the middle across and the middle down are different tiles', and none is near both)
                continue
            (ax, bx, cx), (ay, by, cy) = plane(samples)
            kept = [s for s in samples if abs(ax + bx * s[0] + cx * s[1] - s[2]) <= OFF and abs(ay + by * s[0] + cy * s[1] - s[3]) <= OFF]
            if kept and len(kept) < len(samples):
                (ax, bx, cx), (ay, by, cy) = plane(kept)
            motion[ty, tx] = (ax, ay, 1.0)
    return motion


def refine(f, tiles, label, counts, first):
    """Every tile of a piece that is looked at further, against the field: each of its cells
    shifted by the field at the cell (the field changes within a tile: a constant shift fits a
    fast turn badly at the screen's edge), and that 2 pixels around. -> tiles as around() gives
    them, with the motion at the tile's centre and whether it fits."""
    out = tiles.copy()
    offsets = [(dx, dy) for dy in range(-RR, RR + 1) for dx in range(-RR, RR + 1)]
    offsets.sort(key=lambda d: (abs(d[0]) + abs(d[1]), d[1], d[0]))
    for ty in range(f.th):
        for tx in range(f.tw):
            l = label[ty, tx]
            if l < 0 or tiles[ty, tx, 2] == NONE:
                continue
            if not counts.get(l, False) or first[ty, tx, 2] <= 0:
                if tiles[ty, tx, 2] == FITS:
                    out[ty, tx, 2] = NOT
                continue
            if tiles[ty, tx, 2] == LITTLE:
                continue
            xs, ys = f.cells(tx, ty)
            content, zero = int(tiles[ty, tx, 4]), int(tiles[ty, tx, 5])
            mx, my, found = look_up(first, xs + 1.5, ys + 1.5, first[ty, tx, 0], first[ty, tx, 1])
            mx = np.where(found, mx, first[ty, tx, 0])
            my = np.where(found, my, first[ty, tx, 1])
            sx, sy = np.floor(mx + 0.5).astype(int), np.floor(my + 0.5).astype(int)
            best = None
            for (dx, dy) in offsets:
                c = int(np.abs(f.b_n[ys + PAD, xs + PAD] - f.b_p[np.clip(ys + sy + dy, -PAD, f.h + PAD - 1) + PAD, np.clip(xs + sx + dx, -PAD, f.w + PAD - 1) + PAD]).sum())
                if best is None or c < best[0]:
                    best = (c, dx, dy)
            cost, dx, dy = best
            good = fits(cost, content, zero)
            out[ty, tx] = (int(np.floor(first[ty, tx, 0] + 0.5)) + dx, int(np.floor(first[ty, tx, 1] + 0.5)) + dy, FITS if good else NOT, cost, content, zero)
    return out


def reach(motion):
    """Per tile the rough motions (two at most, further than SAME apart) of tiles whose content
    passes through it; its own first."""
    th, tw, _ = motion.shape
    out = [[[] for _ in range(tw)] for _ in range(th)]
    for ty in range(th):
        for tx in range(tw):
            lst = out[ty][tx]
            order = [(ty, tx)] + [(sy, sx) for sy in range(max(ty - 3, 0), min(ty + 4, th)) for sx in range(max(tx - 4, 0), min(tx + 5, tw)) if (sy, sx) != (ty, tx)]
            for (sy, sx) in order:
                mx, my, valid = motion[sy, sx]
                if not valid:
                    continue
                x0, x1 = sx * TILE + min(mx, 0), sx * TILE + TILE + max(mx, 0)
                y0, y1 = sy * TILE + min(my, 0), sy * TILE + TILE + max(my, 0)
                if x1 <= tx * TILE or x0 >= tx * TILE + TILE or y1 <= ty * TILE or y0 >= ty * TILE + TILE:
                    continue
                if all(abs(mx - q[0]) > SAME or abs(my - q[1]) > SAME for q in lst) and len(lst) < 2:
                    lst.append((float(mx), float(my)))
    return out


def look_up(motion, px, py, rx, ry):
    """The field at pixels (px, py): between the centres of the four tiles around, those with a
    motion within SAME of the rough one (rx, ry). -> mx, my, found"""
    th, tw, _ = motion.shape
    u, v = (px - TILE / 2) / TILE, (py - TILE / 2) / TILE
    i0, j0 = np.floor(u).astype(int), np.floor(v).astype(int)
    fu, fv = u - i0, v - j0
    sum_w = np.zeros(px.shape, dtype=np.float64)
    sum_x = np.zeros(px.shape, dtype=np.float64)
    sum_y = np.zeros(px.shape, dtype=np.float64)
    for dj in (0, 1):
        for di in (0, 1):
            i, j = i0 + di, j0 + dj
            inside = (i >= 0) & (i < tw) & (j >= 0) & (j < th)
            t = motion[np.clip(j, 0, th - 1), np.clip(i, 0, tw - 1)]
            ok = inside & (t[..., 2] > 0) & (np.abs(t[..., 0] - rx) <= SAME) & (np.abs(t[..., 1] - ry) <= SAME)
            w = (fu if di else 1 - fu) * (fv if dj else 1 - fv) + 1e-4
            w = np.where(ok, w, 0.0)
            sum_w += w
            sum_x += w * t[..., 0]
            sum_y += w * t[..., 1]
    found = sum_w > 0
    safe = np.where(found, sum_w, 1.0)
    return sum_x / safe, sum_y / safe, found


SUPPORT = 0.6        # of a pixel's alpha, what the other capture must have at the place it moved from / to
NEAR = 1             # "right next to" a changed pixel: this many pixels away at most (2 explains the game's captures no better)
ALIKE = 0.1          # alphas closer than this are the same thing's


def spread3(a, fn, reach=1):
    """fn (np.maximum / np.logical_or) over each pixel's 3x3 (reach 2: its 5x5)."""
    p = np.pad(a, reach)
    h, w = a.shape
    out = p[reach : reach + h, reach : reach + w]
    for dy in range(2 * reach + 1):
        for dx in range(2 * reach + 1):
            out = fn(out, p[dy : dy + h, dx : dx + w])
    return out


def masks(a_n, a_p, motion):
    """Which pixels are content that moves, in this capture (it was at x + m in the one before)
    and in the one before (it is at x - m in this one). In tiles with a motion:
      this capture's content at a pixel moves when its alpha rose (it arrived); the one before's
      when its alpha fell (it left). The weaker side of a changed pixel (the edge of a stroke over
      where the middle of one was), and a pixel that did not change but has changed ones right
      next to it (text over text), move when the other capture has as much at the place they
      moved from / to; otherwise they are HUD that stands still under or beside what moves. A
      pixel without change around it stays what it is (the inside of a filled shape is the same
      in between).
    -> in_n, in_p (bool), mx, my (the field per pixel, 0 where none)"""
    h, w = a_n.shape
    th, tw, _ = motion.shape
    ys, xs = np.mgrid[0:h, 0:w]
    own = motion[ys // TILE, xs // TILE]
    valid = own[..., 2] > 0
    mx, my, found = look_up(motion, xs.astype(np.float64), ys.astype(np.float64), own[..., 0], own[..., 1])
    valid &= found
    mx, my = np.where(valid, mx, 0.0), np.where(valid, my, 0.0)
    rx, ry = np.floor(mx + 0.5).astype(int), np.floor(my + 0.5).astype(int)
    arrived, left = a_n - a_p > EPS, a_p - a_n > EPS
    near = spread3(arrived | left, np.logical_or, NEAR)
    most_n = np.pad(spread3(a_n, np.maximum), PAD)
    most_p = np.pad(spread3(a_p, np.maximum), PAD)
    came = most_p[np.clip(ys + ry, -PAD, h + PAD - 1) + PAD, np.clip(xs + rx, -PAD, w + PAD - 1) + PAD] >= SUPPORT * a_n
    went = most_n[np.clip(ys - ry, -PAD, h + PAD - 1) + PAD, np.clip(xs - rx, -PAD, w + PAD - 1) + PAD] >= SUPPORT * a_p
    in_n = valid & (a_n > EPS) & (arrived | (came & (left | near)))
    in_p = valid & (a_p > EPS) & (left | (went & (arrived | near)))
    return in_n, in_p, mx, my


def layer_at(a_n, a_p, motion, reached, t, fill=True, alike=True):
    """The layer's alpha for a generated frame at time t, and where it was taken from (0: this
    capture in place, 1: the capture before in place, 2: moved content, 3: nothing, 4: moved
    content that fills a changed place).
      in place  this capture's, unless that is moving content; then the capture before's, unless
                that is moving content too; then nothing.
      moved     for each rough motion that reaches the pixel's tile: the place the field brings
                here by now; what is moving content there goes over what is in place.
      fill      a place whose own content moves, in either capture, also takes what the field
                brings there when that is not seen to move but was, in the capture before, where
                the field says (as much, at that place or right next to it): the inside of a
                filled shape is the same in both captures, and its rear and the places beside a
                moved edge are filled from it. A place that did not change never takes this.
      alike     moved content does not go over what is in place when that is this capture's,
                did not change, is the same along the motion (in the capture before where it
                would have come from, in this one where it would have gone) and is like the moved
                content: it is the inside of the shape that moves, not something under it.
    (fill and alike can be left out, to see what they do: proto/hud-motion/shapes.py, explain.py.)"""
    h, w = a_n.shape
    th, tw, _ = motion.shape
    in_n, in_p, _, _ = masks(a_n, a_p, motion)
    out = np.where(~in_n, a_n, np.where(~in_p, a_p, 0.0))
    kind = np.where(~in_n, 0, np.where(~in_p, 1, 3)).astype(np.uint8)
    pad_n, pad_p = np.pad(a_n, PAD), np.pad(a_p, PAD)
    back = 1.0 - t
    for ty in range(th):
        for tx in range(tw):
            for (rx, ry) in reached[ty][tx]:
                y0, y1 = ty * TILE, min(ty * TILE + TILE, h)
                x0, x1 = tx * TILE, min(tx * TILE + TILE, w)
                ys, xs = np.mgrid[y0:y1, x0:x1]
                gx, gy = xs - back * rx, ys - back * ry
                mx, my, found = look_up(motion, gx, gy, rx, ry)
                sx = np.floor(xs - back * mx + 0.5).astype(int)
                sy = np.floor(ys - back * my + 0.5).astype(int)
                inside = found & (sy >= 0) & (sy < h) & (sx >= 0) & (sx < w)
                sy, sx = np.clip(sy, 0, h - 1), np.clip(sx, 0, w - 1)
                hit = inside & in_n[sy, sx]
                block = out[y0:y1, x0:x1]
                kinds = kind[y0:y1, x0:x1]
                moved = a_n[sy, sx]
                ix, iy = np.floor(mx + 0.5).astype(int), np.floor(my + 0.5).astype(int)
                if alike:
                    here = a_n[y0:y1, x0:x1]
                    came_from = pad_p[np.clip(ys + iy, -PAD, h + PAD - 1) + PAD, np.clip(xs + ix, -PAD, w + PAD - 1) + PAD]
                    went_to = pad_n[np.clip(ys - iy, -PAD, h + PAD - 1) + PAD, np.clip(xs - ix, -PAD, w + PAD - 1) + PAD]
                    inside_shape = (~in_n[y0:y1, x0:x1] & (here > EPS) & (np.abs(here - a_p[y0:y1, x0:x1]) <= EPS)
                                    & (np.abs(came_from - here) <= ALIKE) & (np.abs(went_to - here) <= ALIKE) & (np.abs(moved - here) <= ALIKE))
                    hit = hit & ~inside_shape
                filled = np.zeros(hit.shape, dtype=bool)
                if fill:
                    nearest = np.full(hit.shape, 9.0)
                    for dy in (-1, 0, 1):
                        for dx in (-1, 0, 1):
                            was = pad_p[np.clip(sy + iy + dy, -PAD, h + PAD - 1) + PAD, np.clip(sx + ix + dx, -PAD, w + PAD - 1) + PAD]
                            nearest = np.minimum(nearest, np.abs(was - moved))
                    filled = inside & ~hit & (in_n | in_p)[y0:y1, x0:x1] & ~in_n[sy, sx] & (moved > EPS) & (nearest <= ALIKE)
                block[hit] = (moved + (1.0 - moved) * block)[hit]
                block[filled] = (moved + (1.0 - moved) * block)[filled]
                kinds[hit] = 2
                kinds[filled] = 4
    return out, kind


def track(a_n, a_p, logs=None):
    f = Frame(a_n, a_p)
    log_own = []
    mine, active = own(f, log_own)
    tiles = around(f, mine)
    label, counts, fitting, total, _ = pieces(tiles)
    first = field(tiles, label, counts)
    refined = refine(f, tiles, label, counts, first)
    _, counts2, fitting2, total2, left2 = pieces(refined, label, f)
    motion = field(refined, label, counts2)
    if logs is not None:
        logs.update(own=log_own, active=active, tiles=refined, label=label, counts=counts2, fits=fitting2, total=total2, left=left2,
                    mine=mine, before=tiles, fits1=fitting, first=first)
    return motion, reach(motion)


if __name__ == "__main__":
    print(__doc__)
