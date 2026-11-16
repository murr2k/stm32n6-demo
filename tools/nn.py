#!/usr/bin/env python3
"""Neural networks for the N6's NPU: fetch, convert, flash, check.

  nn.py fetch      download the models (pinned commit, SHA-256 checked) to build-nn/models
  nn.py generate   convert them with ST Edge AI for the N6 into build-nn/<name>/, and
                   write build-nn/nn_weights.h (where each network's weights are, and their CRC)
  nn.py testimg    fetch MediaPipe's hand test pictures, make the palm detector's test input
                   (build-nn/test/palm_in.bin, built into the firmware when present)
  nn.py flash      write the weights to the NPU weight region of the flash (stops the board)
  nn.py check      run the palm detector on the board's test input and compare its outputs
                   with the original model run on the PC (TFLite, from ST Edge AI's Python)
  nn.py inputs     save what the hand tracker's two networks last saw (build-nn/test/live_*.png)
  nn.py trace [NET] [--all]   one run with the epoch trace: time per epoch block against the converter's estimate
  nn.py ref NAME IN.bin OUT.npz   run an original model on the PC on one input

Only this script is committed. The models and test pictures are Apache-2.0
(Google MediaPipe) and are fetched; the converter's output and ST's NPU
runtime are under ST's licence (SLA0044), which forbids putting them under
open-source terms, so they stay in build-nn/ (ignored) and in the ST Edge AI
install. The firmware build picks build-nn/ up when it exists and leaves the
NPU out otherwise.

Memory: each network's working memory (activations) goes to the display's
second frame buffer, 0x3424_9000..0x343C_0000 (the tail of NPU bank 3 and
banks 4 to 6), which the display lends to the NPU while a recognition view is
on screen (it runs single-buffered then); what does not fit spills to a block
at the top of PSRAM. All networks share both, since they run one at a time.
Working memory all in PSRAM was measured on the board at 938 ms for the palm
detector and 350 ms for the hand landmarks, against about 55 and 24 ms
estimated this way. Weights go to their own window of the flash region
0x7580_0000..0x7700_0000.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import sys
import urllib.request
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "build-nn"
STEDGEAI = Path(r"C:\ST\STEdgeAI\4.0")
SE = STEDGEAI / "Utilities" / "windows" / "stedgeai.exe"
ST_PYTHON = STEDGEAI / "Utilities" / "windows" / "python.exe"

HAND_REPO = "https://raw.githubusercontent.com/STMicroelectronics/x-cube-n6-ai-hand-landmarks/" \
            "32fbbd7b84a3572476de250b2252b874a4d086b8/Model/"
TEST_PICTURES = "https://storage.googleapis.com/mediapipe-assets/"
TEST_NAMES = ["thumb_up.jpg", "victory.jpg", "pointing_up.jpg", "fist.jpg"]

# working memory: the top 4 MB of PSRAM (PSRAM_NPU_BASE in psram.h), stopping 64 KB short of
# the end (the last 32 bytes are off limits, errata ES0620 2.4.5)
ACT_BASE = 0x91C00000
ACT_KB = 4032
# frame buffer B (gfx.c), lent to the NPU: the tail of NPU bank 3, then banks 4, 5, 6
NPU_RAM = [("npuRAM3t", "AXISRAM3", 0x34249000, 156), ("npuRAM4", "AXISRAM4", 0x34270000, 448),
           ("npuRAM5", "AXISRAM5", 0x342E0000, 448), ("npuRAM6", "AXISRAM6", 0x34350000, 448)]
NPU_RAM_END = 0x343C0000
# PSRAM's cost for the converter: measured on the board, all-PSRAM runs took about 6 times
# what ST's default ratio of 5 predicts, so the converter is told 30 and keeps hot data out
PSRAM_FREQ_RATIO = 30.0
WEIGHTS_REGION = (0x75800000, 0x77000000)

NETS = {
    "palm_detector": {
        "url": HAND_REPO + "033_palm_detection_full_quant_pc_ff_od.tflite",
        "sha256": "fdf68e7bbe5e9f7e474bb4479df8b95b24ab7474e5ee603ac1427bf6382e0037",
        "weights_at": 0x75800000, "weights_mb": 2,
        "options": "--enable-epoch-controller -O3 --all-buffers-info --cache-maintenance --Oauto-sched "
                   "--native-float --enable-virtual-mem-pools --Omax-ca-pipe 4 --Ocache-opt --Os",
        "args": ["--input-data-type", "uint8"],  # input placed by the converter, on-chip
    },
    "hand_landmark": {
        "url": HAND_REPO + "033_hand_landmark_full_quant_pc_uf_handl.tflite",
        "sha256": "c5ebdcb9aa66848d483aad4d7cd44406c03cbf4fb7275633298a30e6251aa2d9",
        "weights_at": 0x75A00000, "weights_mb": 6,
        "options": "--enable-epoch-controller -O2 --all-buffers-info --cache-maintenance --Oconv-split-kw "
                   "--native-float --enable-virtual-mem-pools --Omax-ca-pipe 4 --Ocache-opt --Os",
        "args": ["--input-data-type", "uint8"],  # input placed by the converter, on-chip
    },
}


def model_path(name: str) -> Path:
    return OUT / "models" / Path(NETS[name]["url"]).name


def cmd_fetch(_: argparse.Namespace) -> None:
    (OUT / "models").mkdir(parents=True, exist_ok=True)
    for name, n in NETS.items():
        p = model_path(name)
        if not p.exists():
            print(f"fetching {p.name}")
            urllib.request.urlretrieve(n["url"], p)
        h = hashlib.sha256(p.read_bytes()).hexdigest()
        if h != n["sha256"]:
            p.unlink()
            sys.exit(f"{p.name}: SHA-256 {h} does not match the pinned {n['sha256']}: deleted")
        print(f"{p.name}: ok")


def mempool(name: str) -> dict:
    """ST Edge AI memory description: working memory in the lent frame buffer, spilling to PSRAM,
    weights in this network's flash window. Bank properties as in ST's own hand application."""
    n = NETS[name]
    cache = {"nlines": 512, "linesize": 64, "associativity": 8, "bypass_enable": 1,
             "prop": {"rights": "ACC_WRITE", "throughput": "MID", "latency": "MID", "byteWidth": 8,
                      "freqRatio": 2.50, "read_power": 13.584, "write_power": 12.645}}
    banks = [{"fname": f, "name": n, "fformat": "FORMAT_RAW",
              "prop": {"rights": "ACC_WRITE", "throughput": "HIGH", "latency": "LOW", "byteWidth": 8,
                       "freqRatio": 1.25, "read_power": 18.531, "write_power": 16.201},
              "offset": {"value": hex(at), "magnitude": "BYTES"}, "size": {"value": str(kb), "magnitude": "KBYTES"}}
             for n, f, at, kb in NPU_RAM]
    psram = {"fname": "xSPI1", "name": "hyperRAM", "fformat": "FORMAT_RAW",
             "prop": {"rights": "ACC_WRITE", "throughput": "MID", "latency": "HIGH", "byteWidth": 2,
                      "freqRatio": PSRAM_FREQ_RATIO, "cacheable": "CACHEABLE_ON", "read_power": 380, "write_power": 340.0,
                      "constants_preferred": "false"},
             "offset": {"value": hex(ACT_BASE), "magnitude": "BYTES"},
             "size": {"value": str(ACT_KB), "magnitude": "KBYTES"}}
    flash = {"fname": "xSPI2", "name": "octoFlash", "fformat": "FORMAT_RAW",
             "prop": {"rights": "ACC_READ", "throughput": "MID", "latency": "HIGH", "byteWidth": 1,
                      "freqRatio": 6.00, "cacheable": "CACHEABLE_ON", "read_power": 110, "write_power": 400.0,
                      "constants_preferred": "true"},
             "offset": {"value": hex(n["weights_at"]), "magnitude": "BYTES"},
             "size": {"value": str(n["weights_mb"]), "magnitude": "MBYTES"}}
    return {"params": {"param": [{"paramname": "max_onchip_sram_size", "value": "0", "magnitude": "KBYTES"}]},
            "memory": {"cacheinfo": [cache], "mem_file_prefix": "atonbuf", "mempools": [*banks, psram, flash]}}


def write_header() -> None:
    """build-nn/nn_weights.h: the firmware checks the weights against these before a first run."""
    lines = ["/* Written by tools/nn.py generate. Where each network's weights are in the flash. */",
             "#ifndef NN_WEIGHTS_H", "#define NN_WEIGHTS_H", "",
             f"#define NN_ACT_BASE 0x{ACT_BASE:08x}u", f"#define NN_ACT_SIZE {ACT_KB * 1024}u",
             f"#define NN_NPU_RAM_BASE 0x{NPU_RAM[0][2]:08x}u", f"#define NN_NPU_RAM_END  0x{NPU_RAM_END:08x}u", ""]
    for name, n in NETS.items():
        w = (OUT / name / "weights.bin").read_bytes()
        u = name.upper()
        lines += [f"#define NN_{u}_WEIGHTS_AT   0x{n['weights_at']:08x}u",
                  f"#define NN_{u}_WEIGHTS_SIZE {len(w)}u",
                  f"#define NN_{u}_WEIGHTS_CRC  0x{zlib.crc32(w):08x}u", ""]
    lines.append("#endif")
    (OUT / "nn_weights.h").write_text("\n".join(lines) + "\n")


def cmd_generate(_: argparse.Namespace) -> None:
    for name, n in NETS.items():
        d = OUT / name
        d.mkdir(parents=True, exist_ok=True)
        (d / "pool.mpool").write_text(json.dumps(mempool(name), indent=1))
        (d / "neuralart.json").write_text(json.dumps(
            {"Globals": {}, "Profiles": {name: {"memory_pool": "./pool.mpool", "options": n["options"]}}}, indent=1))
        shutil.copy(model_path(name), d / model_path(name).name)
        cmd = [str(SE), "generate", "--name", name, "--model", model_path(name).name, "--target", "stm32n6",
               "--st-neural-art", f"{name}@neuralart.json", "--output", "st_ai_output", *n["args"]]
        print(f"generating {name}")
        r = subprocess.run(cmd, cwd=d, capture_output=True, text=True, errors="replace")
        if r.returncode != 0:
            print(r.stdout[-3000:], r.stderr[-3000:])
            sys.exit(f"{name}: stedgeai failed")
        for line in r.stdout.splitlines():
            if any(k in line for k in ("activations (rw)", "weights (ro)", "macc ", "Total number of epochs")):
                print("   ", line.strip())
        o = d / "st_ai_output"
        for f in (f"{name}.c", f"{name}_ecblobs.h", f"stai_{name}.c", f"stai_{name}.h"):
            shutil.copy(o / f, d / f)
        raw = o / f"{name}_atonbuf.xSPI2.raw"
        shutil.copy(raw, d / "weights.bin")
        if raw.stat().st_size > n["weights_mb"] << 20:
            sys.exit(f"{name}: weights {raw.stat().st_size} B do not fit {n['weights_mb']} MB")
    write_header()
    print(f"done: {OUT}")


def letterbox(img, size: int):
    """Scale to fit a size x size square, centred, black bars: what the palm detector expects."""
    from PIL import Image

    img = img.convert("RGB")
    s = size / max(img.size)
    w, h = max(1, round(img.size[0] * s)), max(1, round(img.size[1] * s))
    out = Image.new("RGB", (size, size))
    out.paste(img.resize((w, h), Image.BILINEAR), ((size - w) // 2, (size - h) // 2))
    return out


def palm_anchor(i: int) -> tuple[float, float]:
    """Centre of anchor i of the 192 x 192 palm detector: 24 x 24 cells with 2 anchors, then 12 x 12 with 6."""
    if i < 1152:
        c = i // 2
        return ((c % 24) + .5) / 24, ((c // 24) + .5) / 24
    c = (i - 1152) // 6
    return ((c % 12) + .5) / 12, ((c // 12) + .5) / 12


def hand_roi(scores, boxes) -> tuple[float, float, float, float]:
    """MediaPipe's hand rectangle from the best palm: centre, side (fractions of the square picture) and
    rotation that puts the wrist below the middle finger's knuckle."""
    import math

    i = int(scores.argmax())
    ax, ay = palm_anchor(i)
    r = boxes[i]
    cx, cy, w, h = r[0] / 192 + ax, r[1] / 192 + ay, r[2] / 192, r[3] / 192
    wx, wy = r[4] / 192 + ax, r[5] / 192 + ay
    mx, my = r[8] / 192 + ax, r[9] / 192 + ay
    rot = math.pi / 2 - math.atan2(-(my - wy), mx - wx)
    rot = (rot + math.pi) % (2 * math.pi) - math.pi
    cx += 0.5 * h * math.sin(rot)
    cy -= 0.5 * h * math.cos(rot)
    return cx, cy, max(w, h) * 2.6, rot


def rotated_crop(img, cx: float, cy: float, side: float, rot: float, size: int):
    """size x size bilinear crop of a square picture (H x W x 3 array) around a rotated square."""
    import numpy as np

    n = img.shape[0]
    g = (np.arange(size) + 0.5) / size - 0.5
    u, v = np.meshgrid(g * side * n, g * side * n)
    c, s = np.cos(rot), np.sin(rot)
    x = cx * n + u * c - v * s - 0.5
    y = cy * n + u * s + v * c - 0.5
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = (x - x0)[..., None], (y - y0)[..., None]
    ok = (x0 >= 0) & (y0 >= 0) & (x0 < n - 1) & (y0 < n - 1)
    x0, y0 = np.clip(x0, 0, n - 2), np.clip(y0, 0, n - 2)
    f = img.astype(np.float32)
    p = (f[y0, x0] * (1 - fx) * (1 - fy) + f[y0, x0 + 1] * fx * (1 - fy)
         + f[y0 + 1, x0] * (1 - fx) * fy + f[y0 + 1, x0 + 1] * fx * fy)
    return np.where(ok[..., None], np.clip(p + 0.5, 0, 255), 0).astype(np.uint8)


def cmd_testimg(_: argparse.Namespace) -> None:
    import numpy as np
    from PIL import Image

    d = OUT / "test"
    d.mkdir(parents=True, exist_ok=True)
    for t in TEST_NAMES:
        p = d / t
        if not p.exists():
            print(f"fetching {t}")
            urllib.request.urlretrieve(TEST_PICTURES + t, p)
    pic = Image.open(d / TEST_NAMES[0])
    img = letterbox(pic, 192)
    img.save(d / "palm_in.png")
    (d / "palm_in.bin").write_bytes(img.tobytes())
    run_ref("palm_detector", d / "palm_in.bin", d / "palm_ref.npz")
    z = np.load(d / "palm_ref.npz")
    roi = hand_roi(z["out0"].ravel(), z["out1"].reshape(-1, 18))
    hand = rotated_crop(np.asarray(letterbox(pic, 768)), *roi, 224)
    Image.fromarray(hand).save(d / "hand_in.png")
    (d / "hand_in.bin").write_bytes(hand.tobytes())
    print(f"{d}: palm_in.bin (192 x 192) and hand_in.bin (224 x 224, the hand the PC found) from {TEST_NAMES[0]}")

def cmd_flash(_: argparse.Namespace) -> None:
    import n6

    n6.power_cycle()
    for name, n in NETS.items():
        w = OUT / name / "weights.bin"
        print(f"[nn] writing {name}: {w.stat().st_size / 1e6:.2f} MB at {n['weights_at']:#x}")
        out = n6.cli([*n6.CONNECT, "-el", str(n6.LOADER), "-d", str(w.resolve()), hex(n["weights_at"])])
        if "File download complete" not in out:
            print(out)
            sys.exit("[nn] flash failed")
    print("[nn] done; start the board with: n6.cmd run")


REF = r'''
import sys, numpy as np, tensorflow as tf
model, inp, out, scale, offset = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4]), float(sys.argv[5])
it = tf.lite.Interpreter(model_path=model)
it.allocate_tensors()
d = it.get_input_details()[0]
x = np.fromfile(inp, dtype=np.uint8).reshape(d["shape"])
if d["dtype"] == np.int8:
    x = (x.astype(np.int16) - 128).astype(np.int8)
elif d["dtype"] == np.float32:  # the board's uint8 input means (u - offset) * scale
    x = ((x.astype(np.float32) - offset) * scale).astype(np.float32)
it.set_tensor(d["index"], x)
it.invoke()
res = {}
for i, o in enumerate(it.get_output_details()):
    v = it.get_tensor(o["index"]).astype(np.float32)
    s, z = o["quantization"]
    if s:
        v = (v - z) * s
    res[f"out{i}"] = v
    res[f"lsb{i}"] = np.float32(s or 0)
np.savez(out, **res)
print("reference:", {k: v.shape for k, v in res.items() if k.startswith("out")}, "input", d["dtype"].__name__)
'''


def input_quant(name: str) -> tuple[float, float]:
    """Scale and offset of the converted network's uint8 input, from the converter's description."""
    info = json.loads((OUT / name / "st_ai_output" / f"{name}_c_info.json").read_text())
    first = info["graphs"][0]["inputs"][0]
    b = next(b for b in info["buffers"] if b["id"] == first)
    return b["intq"]["scales"][0], b["intq"]["offsets"][0]


def run_ref(name: str, inp: Path, out: Path) -> None:
    script = OUT / "ref.py"
    script.write_text(REF)
    scale, offset = input_quant(name)
    subprocess.run([str(ST_PYTHON), "-I", str(script), str(model_path(name)), str(inp), str(out), str(scale),
                    str(offset)], check=True)


def cmd_ref(a: argparse.Namespace) -> None:
    run_ref(a.name, Path(a.input), Path(a.output))


def board_read(host: str, addr: int, size: int) -> bytes:
    """Read board memory with the control port's dump command (64 words a line, one connection)."""
    from ctl import send

    cmds, at = [], addr
    while at < addr + size:
        words = min(64, (addr + size - at + 3) // 4)
        cmds.append(f"dump {at:#x} {words}")
        at += words * 4
    data = b""
    for r in send(host, cmds):
        h = r.strip()
        data += b"".join(struct.pack("<I", int(h[i:i + 8], 16)) for i in range(0, len(h), 8))
    return data[:size]


def cmd_check(a: argparse.Namespace) -> None:
    """Each network on its built-in test input, on the board and on the PC: every output compared."""
    import numpy as np
    from ctl import send

    # the NPU's memory is lent while the Camera screen shows; the tracker pauses meanwhile
    send(a.host, ["screen 11"])
    import time

    time.sleep(1.5)
    send(a.host, ["nn hands 0"])
    time.sleep(0.2)
    worst_all = 0.0
    for net, name, inp in ((0, "palm_detector", "palm_in.bin"), (1, "hand_landmark", "hand_in.bin")):
        inp = OUT / "test" / inp
        r = send(a.host, [f"nn test {net}"], timeout=20)[0].strip()
        print(f"{name}: board says {r}")
        if not r.startswith("ok"):
            sys.exit("the board did not run the test")
        f = dict(kv.split("=", 1) for kv in r.split()[1:])
        if int(f["in_crc"], 16) != zlib.crc32(inp.read_bytes()):
            sys.exit(f"the board's input is not {inp} (rebuild the firmware after nn.py testimg)")
        ref = OUT / "test" / f"{name}_ref.npz"
        run_ref(name, inp, ref)
        z = np.load(ref)
        pcs = [z[k].ravel() for k in sorted(z.files) if k.startswith("out")]
        got = {}
        i = 0
        while f"out{i}" in f:
            n = int(f[f"n{i}"])
            b = np.frombuffer(board_read(a.host, int(f[f"out{i}"], 16), n * 4), np.float32)
            same = [p for p in pcs if p.size == n]
            p = min(same, key=lambda p: float(np.abs(p - b).max()))
            d = float(np.abs(p - b).max())
            rng = float(np.abs(p).max()) or 1.0
            worst_all = max(worst_all, d / rng)
            print(f"  output {i}: {n} values, largest difference {d:.4g} ({100 * d / rng:.3f} % of their range)")
            got[n] = (b, p)
            if n <= 4:
                print("    values, board:", [round(float(v), 4) for v in b], " PC:", [round(float(v), 4) for v in p])
            if name == "palm_detector" and n == 2016:
                print("    best anchors, board:", [(int(j), round(float(b[j]), 3)) for j in np.argsort(-b)[:4]])
                print("    best anchors, PC:   ", [(int(j), round(float(p[j]), 3)) for j in np.argsort(-p)[:4]])
            i += 1
        if name == "palm_detector":
            # what the tracker uses: probabilities, and the boxes the PC accepts (score above 0.5)
            (bs, ps), (bb, pb) = got[2016], got[2016 * 18]
            sig = lambda v: 1 / (1 + np.exp(-v.astype(np.float64)))  # noqa: E731
            keep = np.nonzero(ps > 0)[0]
            dbox = np.abs(bb.reshape(-1, 18)[keep] - pb.reshape(-1, 18)[keep]).max() if keep.size else 0.0
            print(f"  probabilities: largest difference {np.abs(sig(bs) - sig(ps)).max():.3f}; "
                  f"{keep.size} anchors above 0.5 on the PC, {int((bs > 0).sum())} on the board; their boxes and "
                  f"key points differ by up to {dbox:.2f} px of 192")
        print(f"  {f['us']} us on the NPU")
    send(a.host, ["nn hands 1"])
    print(f"largest difference of any output: {100 * worst_all:.3f} % of its range")

def cmd_trace(a: argparse.Namespace) -> None:
    """One run of a network with the epoch trace: where its time goes, block by block, against
    the converter's own estimate (NPU at 1 GHz)."""
    from ctl import send

    import time

    send(a.host, ["screen 11"])
    time.sleep(1.5)
    send(a.host, ["nn hands 0"])
    time.sleep(0.2)
    r = send(a.host, [f"nn trace {a.net}"], timeout=20)[0].strip()
    send(a.host, ["nn hands 1"])
    print("board:", r)
    if not r.startswith("ok"):
        sys.exit("no trace")
    f = dict(kv.split("=", 1) for kv in r.split()[1:])
    n, size, hz = int(f["events"]), int(f["size"]), int(f["hz"])
    raw = board_read(a.host, int(f["log"], 16), n * size)
    ev = [struct.unpack_from("<IHHhhII", raw, i * size) for i in range(n)]
    us = lambda c0, c1: ((c1 - c0) & 0xFFFFFFFF) * 1e6 / hz  # noqa: E731
    blocks, cur = [], None
    for cyc, typ, flags, ep0, ep1, est_npu, est_tot in ev:
        if typ == 0:  # PRE_START
            cur = {"t0": cyc, "flags": flags, "ep": (ep0, ep1), "npu": est_npu / 1e3, "tot": est_tot / 1e3}
        elif typ == 2 and cur:  # PRE_END: the hardware part is over
            cur["t2"] = cyc
        elif typ == 3 and cur:  # POST_END
            cur["t3"] = cyc
            blocks.append(cur)
            cur = None
    kinds = {0x10: "hw", 0x20: "sw", 0x40: "hybrid"}
    print(f"{'blk':>3} {'kind':6} {'epochs':>9} {'run us':>9} {'end us':>8} {'gap us':>8} {'est us':>8} {'est cpu':>8}")
    tot = {"run": 0.0, "end": 0.0, "gap": 0.0, "est": 0.0}
    for i, b in enumerate(blocks):
        run = us(b["t0"], b.get("t2", b["t3"]))
        end = us(b.get("t2", b["t3"]), b["t3"])
        gap = us(blocks[i - 1]["t3"], b["t0"]) if i else 0.0
        kind = next((k for m, k in kinds.items() if b["flags"] & m), "?") + ("+blob" if b["flags"] & 4 else "")
        tot["run"] += run
        tot["end"] += end
        tot["gap"] += gap
        tot["est"] += b["tot"]
        if a.all or run + end > 5000:
            print(f"{i:3} {kind:6} {b['ep'][0]:>4}-{b['ep'][1]:<4} {run:9.0f} {end:8.0f} {gap:8.0f} {b['tot']:8.0f} "
                  f"{b['npu']:8.0f}")
    print(f"{len(blocks)} blocks: running {tot['run'] / 1e3:.1f} ms, ending (software) {tot['end'] / 1e3:.1f} ms, "
          f"between blocks {tot['gap'] / 1e3:.1f} ms; converter's estimate {tot['est'] / 1e3:.1f} ms; "
          f"whole run {int(f['us']) / 1e3:.1f} ms")

def cmd_inputs(a: argparse.Namespace) -> None:
    """Pause the hand tracker and save what the two networks last saw as pictures."""
    import time

    from PIL import Image
    from ctl import send

    r = send(a.host, ["nn hands 0"])[0].strip()
    time.sleep(0.3)
    print("board:", r)
    f = dict(kv.split("=", 1) for kv in r.split()[1:])
    d = OUT / "test"
    d.mkdir(parents=True, exist_ok=True)
    for key, n, name in (("in0", 192, "live_palm.png"), ("in1", 224, "live_hand.png")):
        raw = board_read(a.host, int(f[key], 16), n * n * 3)
        Image.frombytes("RGB", (n, n), raw).save(d / name)
        print(d / name)
    send(a.host, ["nn hands 1"])

def main() -> None:
    sys.path.insert(0, str(ROOT / "tools"))
    from ctl import DEFAULT_HOST

    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=DEFAULT_HOST)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name, fn in (("fetch", cmd_fetch), ("generate", cmd_generate), ("testimg", cmd_testimg),
                     ("flash", cmd_flash), ("check", cmd_check), ("inputs", cmd_inputs)):
        sub.add_parser(name).set_defaults(fn=fn)
    sp = sub.add_parser("trace")
    sp.add_argument("net", type=int, nargs="?", default=0, help="0 palm detector, 1 hand landmarks")
    sp.add_argument("--all", action="store_true", help="every block, not only those over 5 ms")
    sp.set_defaults(fn=cmd_trace)
    sp = sub.add_parser("ref")
    sp.add_argument("name")
    sp.add_argument("input")
    sp.add_argument("output")
    sp.set_defaults(fn=cmd_ref)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
