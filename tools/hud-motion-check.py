#!/usr/bin/env python3
"""The layer's passes for HUD content that moves over the screen, against their reference
(proto/hud-motion/hudmotion.py), on captures of the game's HUD layer replayed through the layer:

    TEST_SIZE=2560x1440 TEST_AT="5:DUMP=6" tools/test-chain.sh NAME 12 \\
        "SC_FG_MARKERS=1 SC_FG_MARKER_CAP=100000" --game-ms 30 --hud-files logs/fg/dump-PID 0 7
    tools/hud-motion-check.py logs/test/NAME logs/fg/dump-PID 0 7 [MULTIPLIER]     (default 2)

For every dump of the test with a tiles file (dump-*-markers-WxH.bin): which two captures it was
made from, then each pass by itself, the reference given what the layer's passes before it found
(so that one pass's difference does not show as the next one's), then the whole (the reference
from the captures alone), then the picture: the HUD of the generated frame, read off the dump's
generated picture (white over grey), against the reference's layer at that time.
What may differ: the layer works in single precision where the reference uses double, and its
maps are stored in half precision; a search that is decided by a hair, or a place that rounds the
other way, comes out differently now and then (so in the picture a pixel counts as different only
when none of the reference's pixels right around it is like it). With HUD_STILL="X0 Y0 X1 Y1" (tiles)
nothing in that box may have a motion: the game's map, which must not be moved. With HUD_PICTURES=N
that many pieces, exactly, may be held back as pictures (0: a ship's HUD, all of which is to move).
Prints one block per dump and "ok" or "NOT ok".
"""
import glob, os, re, sys
import numpy as np

HERE = os.path.dirname(os.path.realpath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "proto", "hud-motion"))
import hudmotion as hm

TILE = np.dtype([("own", "<i4", 4), ("around", "<i4", 4), ("sums", "<i4", 4), ("piece", "<f4", 4),
                 ("first", "<f4", 4), ("refined", "<i4", 4), ("motion", "<f4", 4), ("reach", "<f4", 4)])
GREY = (64, 77, 89)  # the test's scene (probe/test_chain.c); some of its rows hold every grey instead


def alpha_of(path):
    w, h = (int(v) for v in re.search(r"-hud-(\d+)x(\d+)\.bin$", path).groups())
    a = np.fromfile(path, dtype=np.float16).reshape(h, w, 4)[:, :, 3].astype(np.float32)
    return np.clip(a, 0.0, 1.0)


def ppm(path):
    data = open(path, "rb").read()
    end = 0
    for _ in range(3):
        end = data.index(b"\n", end) + 1
    w, h = (int(v) for v in data.split(b"\n", 2)[1].split())
    return np.frombuffer(data, dtype=np.uint8, offset=end).reshape(h, w, 3).astype(np.float32)


def share(same, of):
    return f"{int(same.sum())} of {int(of.sum())}" if of.sum() else "none"


def check(prefix, sources, multiplier):
    lines, ok = [], True
    tiles_path = glob.glob(prefix + "-markers-*.bin")[0]
    tw, th = (int(v) for v in re.search(r"-markers-(\d+)x(\d+)\.bin$", tiles_path).groups())
    raw = open(tiles_path, "rb").read()
    counts = np.frombuffer(raw, dtype="<u4", count=4)
    gpu = np.frombuffer(raw, dtype=TILE, offset=16, count=tw * th).reshape(th, tw)
    a_n = alpha_of(glob.glob(prefix + "-hud-*.bin")[0])
    which = [k for k, a in enumerate(sources) if np.array_equal(a, a_n)]
    if not which:
        return [f"{prefix}: its HUD layer is none of the captures given"], False
    k = which[0]
    a_p = sources[(k - 1) % len(sources)]
    f = hm.Frame(a_n, a_p)
    lines.append(f"{os.path.basename(prefix)}: captures {(k - 1) % len(sources)} -> {k}; the layer: {counts[0]} tiles searched, "
                 f"{counts[1]} with a motion, {counts[2]} pieces move, {counts[3]} held back as pictures")
    if os.environ.get("HUD_PICTURES"):
        ok &= int(counts[3]) == int(os.environ["HUD_PICTURES"])

    # ---- each pass by itself
    mine, active = hm.own(f)
    g_own = gpu["own"][:, :, :3].astype(np.int64)
    has = (mine[:, :, 2] != 0) | (g_own[:, :, 2] != 0)
    same = (mine == g_own).all(axis=2)
    lines.append(f"  own search: {share(same & has, has)} tiles the same")
    ok &= (same & has).sum() >= 0.9 * has.sum()

    g_tiles = np.concatenate([gpu["around"].astype(np.int64), gpu["sums"][:, :, :2].astype(np.int64)], axis=2)
    r_tiles = hm.around(f, g_own)
    some = (r_tiles[:, :, 2] != 0) | (g_tiles[:, :, 2] != 0)
    same = (r_tiles == g_tiles).all(axis=2)
    lines.append(f"  around: {share(same & some, some)} tiles the same")
    ok &= bool((same | ~some).all())

    label, counts1, _, _, _ = hm.pieces(g_tiles)
    same = label == gpu["sums"][:, :, 2]
    lines.append(f"  pieces: {int((label >= 0).sum())} tiles in {len(set(label[label >= 0].tolist()))} pieces, names {'the same' if same.all() else 'DIFFER in %d tiles' % int((~same).sum())}")
    ok &= bool(same.all())

    r_first = hm.field(g_tiles, label, counts1)
    g_first = gpu["first"][:, :, :3]
    valid = (r_first[:, :, 2] > 0) == (g_first[:, :, 2] > 0)
    both = (r_first[:, :, 2] > 0) & (g_first[:, :, 2] > 0)
    off = float(np.abs(r_first[:, :, :2] - g_first[:, :, :2])[both].max()) if both.any() else 0.0
    lines.append(f"  field, first round: {int(both.sum())} tiles, {int((~valid).sum())} only in one; largest difference {off:.3f} pixels")
    ok &= bool(valid.all()) and off < 0.05

    g_refined = np.concatenate([gpu["refined"].astype(np.int64), gpu["sums"][:, :, :2].astype(np.int64)], axis=2)
    r_refined = hm.refine(f, g_tiles, label, counts1, g_first.astype(np.float32))
    some = r_tiles[:, :, 2] != 0
    same = (r_refined == g_refined).all(axis=2)
    lines.append(f"  refine: {share(same & some, some)} tiles the same")
    ok &= (same & some).sum() >= 0.97 * some.sum()

    in_piece = label >= 0
    same = gpu["sums"][:, :, 3].astype(np.int64) == f.filled
    lines.append(f"  pixels with content: {share(same & in_piece, in_piece)} tiles the same")
    ok &= bool((same | ~in_piece).all())

    _, counts2, _, _, _ = hm.pieces(g_refined, label, f)
    g_counts2 = {int(l): bool(gpu["piece"].reshape(-1, 4)[int(l), 2] != 0) for l in counts2}
    lines.append(f"  pieces, second round: {sum(counts2.values())} of {len(counts2)} move, {'the same' if g_counts2 == {int(l): bool(v) for l, v in counts2.items()} else 'DIFFERENT'} in the layer")
    ok &= g_counts2 == {int(l): bool(v) for l, v in counts2.items()}

    r_motion = hm.field(g_refined, label, counts2)
    g_motion = gpu["motion"][:, :, :3].astype(np.float32)
    valid = (r_motion[:, :, 2] > 0) == (g_motion[:, :, 2] > 0)
    both = (r_motion[:, :, 2] > 0) & (g_motion[:, :, 2] > 0)
    off = float(np.abs(r_motion[:, :, :2] - g_motion[:, :, :2])[both].max()) if both.any() else 0.0
    lines.append(f"  field: {int(both.sum())} tiles, {int((~valid).sum())} only in one; largest difference {off:.3f} pixels")
    ok &= bool(valid.all()) and off < 0.05

    still = os.environ.get("HUD_STILL")
    if still:
        x0, y0, x1, y1 = (int(v) for v in still.split())
        moved = int((g_motion[y0 : y1 + 1, x0 : x1 + 1, 2] > 0).sum())
        lines.append(f"  tiles ({x0},{y0})-({x1},{y1}), which must stay: {moved} with a motion")
        ok &= moved == 0

    r_reach = hm.reach(g_motion)
    wrong = 0
    for ty in range(th):
        for tx in range(tw):
            n = int(gpu["motion"][ty, tx, 3])
            theirs = [tuple(gpu["reach"][ty, tx, 2 * i : 2 * i + 2]) for i in range(n)]
            wrong += len(theirs) != len(r_reach[ty][tx]) or any(abs(a - b) > 1e-3 for p, q in zip(theirs, r_reach[ty][tx]) for a, b in zip(p, q))
    lines.append(f"  reach: {wrong} tiles differ")
    ok &= wrong == 0

    # ---- the whole, from the captures alone
    motion, _ = hm.track(a_n, a_p)
    one = (motion[:, :, 2] > 0) != (g_motion[:, :, 2] > 0)
    both = (motion[:, :, 2] > 0) & (g_motion[:, :, 2] > 0)
    off = np.abs(motion[:, :, :2] - g_motion[:, :, :2])[both]
    lines.append(f"  the whole: {int(both.sum())} tiles move in both, {int(one.sum())} in one only; difference: "
                 + (f"largest {off.max():.2f}, 95th percentile {np.percentile(off, 95):.2f} pixels" if both.any() else "none"))
    ok &= one.sum() <= max(2, 0.05 * both.sum()) and (not both.any() or np.percentile(off, 95) <= 1.0)

    # ---- the picture
    generated = glob.glob(prefix + "-generated.ppm")
    if generated:
        picture = ppm(generated[0])
        shown = np.clip((picture[:, :, 0] - GREY[0]) / (255.0 - GREY[0]), 0.0, 1.0)
        want, kind = hm.layer_at(a_n, a_p, g_motion, r_reach, 1.0 / multiplier)
        involved = np.repeat(np.repeat((g_motion[:, :, 2] > 0) | np.array([[len(r) > 0 for r in row] for row in r_reach]), 64, axis=0), 64, axis=1)[: a_n.shape[0], : a_n.shape[1]]
        # (the rows of the scene that hold every grey are not part of this: the dump has the scene)
        scene = glob.glob(prefix + "-scene.ppm")
        checked = (np.abs(ppm(scene[0]) - np.array(GREY, dtype=np.float32)) <= 1.0).all(axis=2) if scene else np.ones(a_n.shape, dtype=bool)
        nearest = np.abs(shown - want)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                nearest = np.minimum(nearest, np.abs(shown - np.roll(want, (dy, dx), axis=(0, 1))))
        differs = (nearest > 0.04) & checked
        content = ((want > 0.04) | (shown > 0.04)) & checked
        still = (np.abs(shown - a_n) > 0.04) & checked
        lines.append(f"  picture at 1/{multiplier}: {int((differs & involved).sum())} of {int((content & involved).sum())} HUD pixels in tiles with motion differ from the reference "
                     f"({int((kind == 2).sum())} moved there, {int((kind == 4).sum())} filled from what moved); elsewhere {int((differs & ~involved).sum())} of {int((content & ~involved).sum())}"
                     f"; {int((still & involved & content).sum())} differ from this capture's own layer")
        ok &= (differs & involved).sum() <= 0.02 * max(int((content & involved).sum()), 1) and (differs & ~involved).sum() <= 0.002 * max(int((content & ~involved).sum()), 1)
    else:
        lines.append("  no generated picture in the dump")
        ok = False
    return lines, ok


def main():
    if len(sys.argv) < 5:
        print(__doc__)
        return 2
    folder, source, first, count = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    multiplier = int(sys.argv[5]) if len(sys.argv) > 5 else 2
    sources = [alpha_of(glob.glob(f"{source}-{first + k:03d}-hud-*.bin")[0]) for k in range(count)]
    prefixes = sorted(p[: p.index("-markers-")] for p in glob.glob(os.path.join(folder, "dump-*-markers-*.bin")))
    if not prefixes:
        print(f"{folder}: no dump with a tiles file (was SC_FG_MARKERS=1 set, and the dump asked for?)")
        return 1
    bad = 0
    for prefix in prefixes:
        lines, ok = check(prefix, sources, multiplier)
        print("\n".join(lines))
        print("  ok" if ok else "  NOT ok")
        bad += not ok
    return 1 if bad else 0


sys.exit(main())
