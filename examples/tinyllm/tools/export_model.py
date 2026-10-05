#!/usr/bin/env python3
"""export_model.py — bake a llama2-family checkpoint into a `.phxllm` blob for examples/tinyllm.

This script is the HOST half of the tinyllm example. It does every expensive, fallible thing
offline (Phosphorus's pipeline philosophy, docs/08): quantization, the RoPE table, the exp/SiLU
lookup tables, the tokenizer index — so the Game Boy Advance only ever streams int8 out of
cartridge ROM.

It also carries a **bit-exact Python mirror of the C++ inference core** (see FIXED REFERENCE
below). That mirror is what makes the golden-token test meaningful: two independent
implementations of the same integer arithmetic must agree exactly, which catches the class of
fixed-point bug that is otherwise impossible to isolate once it reaches a ROM.

Usage
-----
  # bake a checkpoint (llama2.c legacy .bin + its tokenizer)
  export_model.py export --checkpoint stories260K.bin --tokenizer tok512.bin \
                         --out model.phxllm [--group-size 32]

  # the same, plus a golden-token file the C++ suite asserts against
  export_model.py export --checkpoint ... --tokenizer ... --out model.phxllm \
                         --golden tests/fixtures/tinyllm_golden.txt \
                         --prompt "Once upon a time" --steps 24

  # the tiny random fixture the unit/integration suites use (no checkpoint needed)
  export_model.py fixture --out tests/fixtures/tinyllm_tiny.phxllm \
                          --golden tests/fixtures/tinyllm_tiny_golden.txt

  # report quantization damage as a perplexity delta on a held-out sample
  export_model.py eval --checkpoint ... --tokenizer ...

Format
------
See examples/tinyllm/src/llm_format.h — it is the normative definition and this file mirrors it.
Every constant below is restated from that header; if you change one, change both (the C++ side
static_asserts its struct sizes, and tests/unit/test_tinyllm_format.cpp pins the layout).
"""

import argparse
import math
import os
import struct
import sys

import numpy as np

# ---------------------------------------------------------------------------------------------
# Format constants — mirror of examples/tinyllm/src/llm_format.h
# ---------------------------------------------------------------------------------------------

LLM_MAGIC = 0x4C584850  # 'PHXL'
LLM_VERSION_MAJOR = 1
LLM_VERSION_MINOR = 0

EXP_LUT_N = 512
EXP_LUT_MIN = -(16 << 16)
EXP_LUT_SHIFT = 11          # step 1/32 in Q16.16
SILU_LUT_N = 512
SILU_LUT_MIN = -(16 << 16)
SILU_LUT_SHIFT = 12         # step 1/16 in Q16.16

FLAG_SHARED_CLASSIFIER = 1 << 0

(K_TOK_EMB, K_RMS_ATT, K_WQ, K_WK, K_WV, K_WO, K_RMS_FFN, K_W1, K_W2, K_W3,
 K_RMS_FINAL, K_WCLS, K_ROPE_COS, K_ROPE_SIN, K_EXP_LUT, K_SILU_LUT,
 K_TOK_BYTES, K_TOK_INDEX, K_TOK_SORTED, K_TOK_SCORE) = range(20)

DT_Q8, DT_I16, DT_I32, DT_U8, DT_U16, DT_U32 = range(6)
DT_SIZE = {DT_Q8: 1, DT_I16: 2, DT_I32: 4, DT_U8: 1, DT_U16: 2, DT_U32: 4}

NO_LAYER = 0xFFFF
HEADER_FMT = "<IHH" + "H" * 10 + "hH" + "iIIII" + "HHHHI"
HEADER_SIZE = 64
TENSOR_FMT = "<IBBHIIIIII"
TENSOR_SIZE = 32

assert struct.calcsize(HEADER_FMT) == HEADER_SIZE
assert struct.calcsize(TENSOR_FMT) == TENSOR_SIZE

Q16_ONE = 1 << 16


def fnv1a(s):
    h = 0x811C9DC5
    for b in s.encode("utf-8"):
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def crc32_of(data):
    import zlib
    return zlib.crc32(data) & 0xFFFFFFFF


# ---------------------------------------------------------------------------------------------
# FIXED-POINT PRIMITIVES — bit-exact mirror of examples/tinyllm/src/fixedmath.h
#
# Python's `>>` floors, which is what an arithmetic shift right does on every compiler this
# project builds with, so these produce the identical integers the C++ core does. Every
# intermediate is bounded so Python's arbitrary precision never diverges from int64.
# ---------------------------------------------------------------------------------------------

RECIP_SEED_A = (48 << 30) // 17
RECIP_SEED_B = (32 << 30) // 17


def sat32(v):
    if v > 2147483647:
        return 2147483647
    if v < -2147483648:
        return -2147483648
    return int(v)


def q_clz32(v):
    assert 0 < v < (1 << 32)
    return 32 - v.bit_length()


def q_recip(d):
    """1/d ~= m * 2^-sh, m in (2^30, 2^31]. Newton-Raphson, no division."""
    assert d > 0
    lz = q_clz32(d)
    b = (d << lz) & 0xFFFFFFFF
    x = RECIP_SEED_A - ((RECIP_SEED_B * b) >> 32)
    for _ in range(4):
        fx = (b * x) >> 32
        x = (x * ((2 << 30) - fx)) >> 30
    return x, 62 - lz


def q_div(n, d):
    if d == 0:
        return 2147483647 if n >= 0 else -2147483648
    neg = (n < 0) != (d < 0)
    m, sh = q_recip(abs(d))
    q = (abs(n) * m) >> (sh - 16)
    return sat32(-q if neg else q)


def q_mul(a, b):
    return sat32((a * b) >> 16)


def q_scale_q30(acc, s):
    if acc == 0 or s == 0:
        return 0
    neg = acc < 0
    a = -acc if neg else acc
    sh = 30
    while a >= (1 << 31):
        a >>= 1
        sh -= 1
    if sh < 0:
        return -2147483648 if neg else 2147483647
    p = (a * s) >> sh
    return sat32(-p if neg else p)


def q_sqrt(a):
    return 0 if a <= 0 else math.isqrt(a << 16)


def q_rsqrt(a):
    s = q_sqrt(a)
    return 2147483647 if s <= 0 else q_div(Q16_ONE, s)


def q_exp_neg(lut, x):
    if x >= 0:
        return Q16_ONE
    if x <= -(16 << 16):
        return 0
    t = x + (16 << 16)
    i = t >> EXP_LUT_SHIFT
    frac = t & ((1 << EXP_LUT_SHIFT) - 1)
    if i >= EXP_LUT_N:
        return int(lut[EXP_LUT_N])
    return int(lut[i]) + (((int(lut[i + 1]) - int(lut[i])) * frac) >> EXP_LUT_SHIFT)


def q_silu(lut, x):
    if x >= (16 << 16):
        return x
    if x <= -(16 << 16):
        return 0
    t = x + (16 << 16)
    i = t >> SILU_LUT_SHIFT
    frac = t & ((1 << SILU_LUT_SHIFT) - 1)
    if i >= SILU_LUT_N:
        return int(lut[SILU_LUT_N])
    return int(lut[i]) + (((int(lut[i + 1]) - int(lut[i])) * frac) >> SILU_LUT_SHIFT)


def q_quantize(x, n, stride):
    """Mirror of q_quantize(): int8 codes + the Q30 scale. `x` is a list/array of Q16.16 ints."""
    out = np.zeros(stride, dtype=np.int8)
    xs = [int(v) for v in x[:n]]
    maxabs = max((abs(v) for v in xs), default=0)
    if maxabs <= 0:
        return out, 0
    m, sh = q_recip(maxabs)
    m127, sh127 = q_recip(127)
    scale_q30 = sat32((maxabs * m127) >> (sh127 - 14))
    mult = (m * 127) >> 7
    msh = sh - 7
    half = 1 << (msh - 1)
    for i, v in enumerate(xs):
        q = ((abs(v) * mult) + half) >> msh
        if q > 127:
            q = 127
        out[i] = -q if v < 0 else q
    return out, scale_q30


def build_exp_lut():
    return np.array([int(round(math.exp(-16.0 + i / 32.0) * 65536.0))
                     for i in range(EXP_LUT_N + 1)], dtype=np.int32)


def build_silu_lut():
    def silu(v):
        return v / (1.0 + math.exp(-v))
    return np.array([int(round(silu(-16.0 + i / 16.0) * 65536.0))
                     for i in range(SILU_LUT_N + 1)], dtype=np.int32)


# ---------------------------------------------------------------------------------------------
# Checkpoint loading
# ---------------------------------------------------------------------------------------------

class Checkpoint:
    """A llama2-family checkpoint held as float32 numpy arrays, in (out, in) row-major order."""

    dim: int
    hidden_dim: int
    n_layers: int
    n_heads: int
    n_kv_heads: int
    vocab_size: int
    seq_len: int

    def __init__(self, cfg, tensors, shared):
        self.__dict__.update(cfg)
        self.t = tensors
        self.shared = shared
        self.head_size = self.dim // self.n_heads
        self.kv_dim = self.head_size * self.n_kv_heads

    def describe(self):
        p = sum(int(np.prod(v.shape)) for k, v in self.t.items() if not k.startswith("freq"))
        return (f"dim={self.dim} hidden={self.hidden_dim} layers={self.n_layers} "
                f"heads={self.n_heads} kv_heads={self.n_kv_heads} vocab={self.vocab_size} "
                f"seq={self.seq_len} head_size={self.head_size} params={p}")


def load_llama2c_bin(path):
    """The llama2.c legacy export: 7 int32 config words then float32 weights, in a fixed order.
    A positive vocab_size flags a classifier that shares the embedding table."""
    with open(path, "rb") as f:
        dim, hidden_dim, n_layers, n_heads, n_kv_heads, vocab_size, seq_len = \
            struct.unpack("<7i", f.read(28))
        shared = vocab_size > 0
        vocab_size = abs(vocab_size)
        raw = np.fromfile(f, dtype=np.float32)

    head_size = dim // n_heads
    kv_dim = head_size * n_kv_heads
    off = 0

    def take(*shape):
        nonlocal off
        n = int(np.prod(shape))
        if off + n > raw.size:
            raise SystemExit(f"checkpoint truncated: wanted {n} floats at {off}, have {raw.size}")
        a = raw[off:off + n].reshape(shape).astype(np.float32)
        off += n
        return a

    t = {}
    t["tok_emb"] = take(vocab_size, dim)
    t["rms_att"] = take(n_layers, dim)
    t["wq"] = take(n_layers, dim, dim)
    t["wk"] = take(n_layers, kv_dim, dim)
    t["wv"] = take(n_layers, kv_dim, dim)
    t["wo"] = take(n_layers, dim, dim)
    t["rms_ffn"] = take(n_layers, dim)
    t["w1"] = take(n_layers, hidden_dim, dim)
    t["w2"] = take(n_layers, dim, hidden_dim)
    t["w3"] = take(n_layers, hidden_dim, dim)
    t["rms_final"] = take(dim)
    t["freq_real"] = take(seq_len, head_size // 2)
    t["freq_imag"] = take(seq_len, head_size // 2)
    if not shared:
        t["wcls"] = take(vocab_size, dim)
    if off != raw.size:
        print(f"  ! warning: {raw.size - off} trailing floats unread", file=sys.stderr)

    cfg = dict(dim=dim, hidden_dim=hidden_dim, n_layers=n_layers, n_heads=n_heads,
               n_kv_heads=n_kv_heads, vocab_size=vocab_size, seq_len=seq_len)
    return Checkpoint(cfg, t, shared)


def load_state_dict(path):
    """A PyTorch checkpoint (llama2.c training output or an equivalent state_dict). Needs torch;
    the legacy .bin path above needs nothing but numpy."""
    try:
        import torch     # type: ignore[import-not-found]  (optional; the .bin path needs no torch)
    except ImportError:
        raise SystemExit("--checkpoint is a .pt/.pth but PyTorch is not installed; "
                         "export the llama2.c legacy .bin instead")
    obj = torch.load(path, map_location="cpu", weights_only=False)
    sd = obj.get("model", obj) if isinstance(obj, dict) else obj
    args = obj.get("model_args", {}) if isinstance(obj, dict) else {}
    sd = {k.replace("_orig_mod.", ""): v for k, v in sd.items()}

    def g(name):
        return sd[name].float().numpy()

    dim = args.get("dim") or g("tok_embeddings.weight").shape[1]
    n_layers = args.get("n_layers") or len({k.split(".")[1] for k in sd if k.startswith("layers.")})
    n_heads = args["n_heads"]
    n_kv_heads = args.get("n_kv_heads") or n_heads
    vocab_size = g("tok_embeddings.weight").shape[0]
    hidden_dim = g("layers.0.feed_forward.w1.weight").shape[0]
    seq_len = args.get("max_seq_len", 512)

    t = {}
    t["tok_emb"] = g("tok_embeddings.weight")
    t["rms_att"] = np.stack([g(f"layers.{i}.attention_norm.weight") for i in range(n_layers)])
    t["rms_ffn"] = np.stack([g(f"layers.{i}.ffn_norm.weight") for i in range(n_layers)])
    for nm, key in (("wq", "attention.wq"), ("wk", "attention.wk"), ("wv", "attention.wv"),
                    ("wo", "attention.wo"), ("w1", "feed_forward.w1"),
                    ("w2", "feed_forward.w2"), ("w3", "feed_forward.w3")):
        t[nm] = np.stack([g(f"layers.{i}.{key}.weight") for i in range(n_layers)])
    t["rms_final"] = g("norm.weight")
    shared = "output.weight" not in sd
    if not shared:
        t["wcls"] = g("output.weight")

    cfg = dict(dim=dim, hidden_dim=hidden_dim, n_layers=n_layers, n_heads=n_heads,
               n_kv_heads=n_kv_heads, vocab_size=vocab_size, seq_len=seq_len)
    return Checkpoint(cfg, t, shared)


def load_checkpoint(path):
    return (load_state_dict(path) if path.endswith((".pt", ".pth"))
            else load_llama2c_bin(path))


# ---------------------------------------------------------------------------------------------
# Tokenizer
# ---------------------------------------------------------------------------------------------

class Tokenizer:
    def __init__(self, pieces, scores, bos=1, eos=2):
        self.pieces = pieces                     # list[bytes]
        self.scores = scores                     # list[float]
        self.bos, self.eos = bos, eos
        self.max_len = max(len(p) for p in pieces)
        # Deterministic sort: bytes first, then id, so duplicate pieces resolve to the lowest id
        # and the runtime's lower_bound binary search finds the same entry the exporter did.
        self.sorted_ids = sorted(range(len(pieces)), key=lambda i: (pieces[i], i))
        self._lookup = {}
        for i in reversed(self.sorted_ids):      # reversed => the lowest id wins
            self._lookup[pieces[i]] = i

    def lookup(self, b):
        return self._lookup.get(b, -1)

    def encode(self, text, add_bos=True):
        """llama2.c's BPE encoder, reproduced exactly (the C++ llm_encode mirrors this)."""
        toks = []
        if add_bos:
            toks.append(self.bos)
        if text:
            sp = self.lookup(b" ")
            if sp >= 0:
                toks.append(sp)
        raw = text.encode("utf-8")
        i = 0
        while i < len(raw):
            n = 1
            if raw[i] & 0xC0 == 0xC0:
                while n < 4 and i + n < len(raw) and (raw[i + n] & 0xC0) == 0x80:
                    n += 1
            piece = raw[i:i + n]
            tid = self.lookup(piece)
            if tid >= 0:
                toks.append(tid)
            else:
                toks.extend(b + 3 for b in piece)
            i += n
        while True:
            best_score, best_id, best_idx = -1e30, -1, -1
            for j in range(len(toks) - 1):
                merged = self.pieces[toks[j]] + self.pieces[toks[j + 1]]
                if len(merged) > self.max_len:
                    continue
                tid = self.lookup(merged)
                if tid >= 0 and self.scores[tid] > best_score:
                    best_score, best_id, best_idx = self.scores[tid], tid, j
            if best_idx < 0:
                break
            toks[best_idx] = best_id
            del toks[best_idx + 1]
        return toks

    def decode(self, prev, tok):
        p = self.pieces[tok]
        if prev == self.bos and p.startswith(b" "):
            p = p[1:]
        if len(p) == 6 and p[0:3] == b"<0x" and p[5:6] == b">":
            try:
                return bytes([int(p[3:5], 16)])
            except ValueError:
                pass
        return p


def load_tokenizer(path):
    """llama2.c tokenizer.bin: max_token_length, then (float score, int len, bytes) per token."""
    with open(path, "rb") as f:
        data = f.read()
    off = 0
    (_max_len,) = struct.unpack_from("<i", data, off)
    off += 4
    pieces, scores = [], []
    while off < len(data):
        (score,) = struct.unpack_from("<f", data, off)
        off += 4
        (ln,) = struct.unpack_from("<i", data, off)
        off += 4
        pieces.append(data[off:off + ln])
        scores.append(float(score))
        off += ln
    return Tokenizer(pieces, scores)


def build_fixture_tokenizer():
    """A synthetic vocab shaped exactly like tok512: 3 specials, 256 byte tokens, then printable
    ASCII singles and a handful of multi-byte merges — enough to exercise the merge loop, the
    binary search, and the <0xXX> byte-fallback path in a blob small enough to commit."""
    pieces = [b"<unk>", b"\n<s>\n", b"\n</s>\n"]
    scores = [0.0, 0.0, 0.0]
    for b in range(256):
        pieces.append(("<0x%02X>" % b).encode())
        scores.append(0.0)
    extra = [bytes([c]) for c in range(0x20, 0x7F)]
    extra += [b" t", b"he", b" a", b"in", b" the", b"ed", b" to", b" and", b"er", b"on",
              b" w", b"nd", b"ll", b" s", b"or", b"an", b" was", b"it", b"ay", b" he",
              b" that", b"ing", b" said", b"ar", b"om", b" f", b"ow", b" b", b"is", b"en"]
    for rank, p in enumerate(extra):
        pieces.append(p)
        scores.append(-float(rank))
    return Tokenizer(pieces, scores)


# ---------------------------------------------------------------------------------------------
# Quantization
# ---------------------------------------------------------------------------------------------

def quantize_rows(mat, group):
    """Symmetric int8 with a per-group Q30 scale. Rows are padded up to a whole group with zeros
    so the device kernel never has a ragged tail (the padded activations are zeros, so the
    padding contributes nothing). Returns (int8 [rows, stride], int32 [rows, groups], err)."""
    mat = np.asarray(mat, dtype=np.float32)
    if mat.ndim == 1:
        mat = mat.reshape(1, -1)
    rows, cols = mat.shape
    stride = ((cols + group - 1) // group) * group
    padded = np.zeros((rows, stride), dtype=np.float32)
    padded[:, :cols] = mat
    g = padded.reshape(rows, stride // group, group)

    absmax = np.abs(g).max(axis=2)
    scale = absmax / 127.0
    safe = np.where(scale > 0, scale, 1.0)
    q = np.rint(g / safe[:, :, None]).astype(np.int32)
    q = np.clip(q, -127, 127).astype(np.int8)
    q[absmax == 0] = 0

    # Q30 is deliberate: a weight group scale here measures ~1e-4, and Q16.16's 1.5e-5 step would
    # round that to ~0.9% relative error (measured on stories260K). Q30's step is 9.3e-10.
    scale_q30 = np.clip(np.rint(scale * (1 << 30)), 0, 2147483647).astype(np.int32)

    deq = q.astype(np.float32) * (scale_q30.astype(np.float64) / (1 << 30))[:, :, None]
    err = float(np.abs(deq - g).max())
    rel = float(np.sqrt(np.mean((deq - g) ** 2)) / (np.sqrt(np.mean(g ** 2)) + 1e-12))
    return q.reshape(rows, stride), scale_q30.reshape(rows, -1), (err, rel)


def dequantize(q, s, group, cols):
    """Exactly what the device does when it reads a weight back (used by the fixed reference)."""
    rows, stride = q.shape
    g = q.reshape(rows, stride // group, group).astype(np.float64)
    return (g * (s.astype(np.float64) / (1 << 30))[:, :, None]).reshape(rows, stride)[:, :cols]


# ---------------------------------------------------------------------------------------------
# Blob writer
# ---------------------------------------------------------------------------------------------

class BlobWriter:
    def __init__(self):
        self.entries = []       # (name, kind, dtype, layer, rows, cols, stride, payload, scales)

    def add(self, name, kind, dtype, rows, cols, stride, payload, layer=NO_LAYER, scales=None):
        assert len(payload) == rows * stride * DT_SIZE[dtype], (name, len(payload))
        self.entries.append(dict(name=name, kind=kind, dtype=dtype, layer=layer, rows=rows,
                                 cols=cols, stride=stride, payload=payload, scales=scales))

    def add_q8(self, name, kind, q, s, cols, layer=NO_LAYER):
        rows, stride = q.shape
        self.add(name, kind, DT_Q8, rows, cols, stride, q.tobytes(), layer,
                 np.ascontiguousarray(s, dtype="<i4").tobytes())

    def add_vec(self, name, kind, arr, dtype, layer=NO_LAYER):
        a = np.ascontiguousarray(arr).ravel()
        code = {DT_I16: "<i2", DT_I32: "<i4", DT_U8: "u1", DT_U16: "<u2", DT_U32: "<u4"}[dtype]
        blob = a.astype(code).tobytes()
        self.add(name, kind, dtype, 1, a.size, a.size, blob, layer)

    def add_bytes(self, name, kind, data, layer=NO_LAYER):
        self.add(name, kind, DT_U8, 1, len(data), len(data), bytes(data), layer)

    def write(self, path, hdr):
        count = len(self.entries)
        dir_off = HEADER_SIZE
        cursor = dir_off + count * TENSOR_SIZE
        payloads = []

        def place(data):
            nonlocal cursor
            pad = (-cursor) % 4
            if pad:
                payloads.append(b"\0" * pad)
                cursor += pad
            off = cursor
            payloads.append(data)
            cursor += len(data)
            return off

        directory = []
        for e in self.entries:
            data_off = place(e["payload"])
            scale_off = place(e["scales"]) if e["scales"] else 0
            directory.append(struct.pack(TENSOR_FMT, fnv1a(e["name"]), e["kind"], e["dtype"],
                                         e["layer"], e["rows"], e["cols"], e["stride"],
                                         data_off, scale_off, len(e["payload"])))
        blob_size = cursor
        head = struct.pack(
            HEADER_FMT, LLM_MAGIC, LLM_VERSION_MAJOR, LLM_VERSION_MINOR,
            hdr["dim"], hdr["hidden_dim"], hdr["n_layers"], hdr["n_heads"], hdr["n_kv_heads"],
            hdr["vocab_size"], hdr["seq_len"], hdr["group_size"], hdr["head_size"], hdr["kv_dim"],
            hdr["kv_shift"], hdr["flags"], hdr["rms_eps_q16"], count, dir_off, blob_size, 0,
            hdr["max_token_len"], hdr["bos_id"], hdr["eos_id"], 0, 0)
        body = b"".join(directory) + b"".join(payloads)
        blob = bytearray(head + body)
        assert len(blob) == blob_size, (len(blob), blob_size)
        crc = crc32_of(bytes(blob[52:]))
        struct.pack_into("<I", blob, 48, crc)
        with open(path, "wb") as f:
            f.write(blob)
        return bytes(blob)


def build_rope_tables(seq_len, head_size):
    """cos/sin for every (position, frequency pair), Q15 int16. Baked so the device never calls
    sin/cos — and so both scalar tiers see the identical table."""
    half = head_size // 2
    cos = np.zeros((seq_len, half), dtype=np.int16)
    sin = np.zeros((seq_len, half), dtype=np.int16)
    for pos in range(seq_len):
        for j in range(half):
            freq = 1.0 / (10000.0 ** ((2 * j) / head_size))
            v = pos * freq
            cos[pos, j] = int(np.clip(round(math.cos(v) * 32768.0), -32768, 32767))
            sin[pos, j] = int(np.clip(round(math.sin(v) * 32768.0), -32768, 32767))
    return cos, sin


def pick_kv_shift(maxkv):
    """Choose the KV cache's Q shift from calibration: the largest s that keeps the observed
    maximum inside int16 with ~2x headroom, so a slightly out-of-distribution prompt still
    cannot saturate the cache."""
    if maxkv <= 0:
        return 12
    s = int(math.floor(math.log2(32767.0 / (maxkv * 2.0))))
    return max(1, min(16, s))


# ---------------------------------------------------------------------------------------------
# Blob reader — the inverse of BlobWriter, so a `.phxllm` produced by ANY writer (including the
# C++ one in examples/tinyllm/src/llm_build.h, which builds the test fixture) can be loaded back
# and driven through the references below. This is what lets the committed golden be text only:
# the fixture blob is generated on the fly by C++, and this reads those exact bytes.
# ---------------------------------------------------------------------------------------------

def load_blob(path_or_bytes):
    blob = (open(path_or_bytes, "rb").read() if isinstance(path_or_bytes, str)
            else bytes(path_or_bytes))
    f = struct.unpack_from(HEADER_FMT, blob, 0)
    keys = ("magic", "version_major", "version_minor", "dim", "hidden_dim", "n_layers",
            "n_heads", "n_kv_heads", "vocab_size", "seq_len", "group_size", "head_size",
            "kv_dim", "kv_shift", "flags", "rms_eps_q16", "tensor_count", "dir_offset",
            "blob_size", "crc32", "max_token_len", "bos_id", "eos_id", "_r0", "_r1")
    hdr = dict(zip(keys, f))
    if hdr["magic"] != LLM_MAGIC:
        raise SystemExit(f"{path_or_bytes}: not a .phxllm (bad magic)")
    if hdr["version_major"] != LLM_VERSION_MAJOR:
        raise SystemExit(f"{path_or_bytes}: version {hdr['version_major']} unsupported")
    if hdr["blob_size"] != len(blob):
        raise SystemExit(f"{path_or_bytes}: blob_size {hdr['blob_size']} != file {len(blob)}")

    group = hdr["group_size"]
    tensors = {}
    for i in range(hdr["tensor_count"]):
        off = hdr["dir_offset"] + i * TENSOR_SIZE
        (_name, kind, dtype, layer, rows, cols, stride, data_off, scale_off, nbytes) = \
            struct.unpack_from(TENSOR_FMT, blob, off)
        code = {DT_Q8: "i1", DT_I16: "<i2", DT_I32: "<i4",
                DT_U8: "u1", DT_U16: "<u2", DT_U32: "<u4"}[dtype]
        data = np.frombuffer(blob, dtype=code, count=rows * stride,
                             offset=data_off).reshape(rows, stride)
        scales = None
        if scale_off:
            scales = np.frombuffer(blob, dtype="<i4", count=rows * (stride // group),
                                   offset=scale_off).reshape(rows, stride // group)
        tensors.setdefault(kind, {})[layer] = dict(data=data, scales=scales,
                                                   rows=rows, cols=cols, stride=stride)

    def one(kind):
        return tensors[kind][NO_LAYER]

    def q(kind, layer=NO_LAYER):
        t = tensors[kind][layer]
        return (np.ascontiguousarray(t["data"]), np.ascontiguousarray(t["scales"]))

    n_layers = hdr["n_layers"]
    qw: dict = {"tok_emb": q(K_TOK_EMB)}
    qw["cls"] = qw["tok_emb"] if (hdr["flags"] & FLAG_SHARED_CLASSIFIER) else q(K_WCLS)
    for nm, kind in (("wq", K_WQ), ("wk", K_WK), ("wv", K_WV), ("wo", K_WO),
                     ("w1", K_W1), ("w2", K_W2), ("w3", K_W3)):
        qw[nm] = [q(kind, l) for l in range(n_layers)]
    qw["rms_att"] = [tensors[K_RMS_ATT][l]["data"].ravel() for l in range(n_layers)]
    qw["rms_ffn"] = [tensors[K_RMS_FFN][l]["data"].ravel() for l in range(n_layers)]
    qw["rms_final"] = one(K_RMS_FINAL)["data"].ravel()

    luts = (one(K_EXP_LUT)["data"].ravel(), one(K_SILU_LUT)["data"].ravel())
    half = hdr["head_size"] // 2
    rope = (one(K_ROPE_COS)["data"].reshape(-1, half), one(K_ROPE_SIN)["data"].reshape(-1, half))

    tb = one(K_TOK_BYTES)["data"].tobytes()
    ti = one(K_TOK_INDEX)["data"].ravel()
    ts = one(K_TOK_SCORE)["data"].ravel()
    pieces = [tb[int(ti[i]):int(ti[i + 1])] for i in range(hdr["vocab_size"])]
    tok = Tokenizer(pieces, [float(v) / 65536.0 for v in ts],
                    bos=hdr["bos_id"], eos=hdr["eos_id"])
    return blob, hdr, qw, luts, rope, tok


def dequantized_checkpoint(hdr, qw):
    """Rebuild a float Checkpoint from a blob's quantized weights. For the random fixture there
    is no original fp32 to compare against, so 'float' means 'the same weights, evaluated in
    float32' — it still measures how much the integer pipeline itself drifts."""
    g = hdr["group_size"]
    dim, hidden, nl = hdr["dim"], hdr["hidden_dim"], hdr["n_layers"]

    def d(pair, cols):
        return dequantize(pair[0], pair[1], g, cols).astype(np.float32)

    t = {"tok_emb": d(qw["tok_emb"], dim),
         "rms_att": np.stack([np.asarray(v, dtype=np.float64) / 65536.0
                              for v in qw["rms_att"]]).astype(np.float32),
         "rms_ffn": np.stack([np.asarray(v, dtype=np.float64) / 65536.0
                              for v in qw["rms_ffn"]]).astype(np.float32),
         "rms_final": (np.asarray(qw["rms_final"], dtype=np.float64) / 65536.0
                       ).astype(np.float32)}
    for nm, cols in (("wq", dim), ("wk", dim), ("wv", dim), ("wo", dim),
                     ("w1", dim), ("w2", hidden), ("w3", dim)):
        t[nm] = np.stack([d(qw[nm][l], cols) for l in range(nl)])
    cfg = dict(dim=dim, hidden_dim=hidden, n_layers=nl, n_heads=hdr["n_heads"],
               n_kv_heads=hdr["n_kv_heads"], vocab_size=hdr["vocab_size"],
               seq_len=hdr["seq_len"])
    return Checkpoint(cfg, t, shared=bool(hdr["flags"] & FLAG_SHARED_CLASSIFIER))


# ---------------------------------------------------------------------------------------------
# FLOAT REFERENCE — llama2.c's run.c forward pass, in float32. The measuring stick.
# ---------------------------------------------------------------------------------------------

class FloatModel:
    def __init__(self, ck, rms_eps=1e-5):
        self.ck = ck
        self.eps = rms_eps
        self.max_seq = ck.seq_len
        self.reset()

    def reset(self):
        ck = self.ck
        self.kc = np.zeros((ck.n_layers, self.max_seq, ck.kv_dim), dtype=np.float32)
        self.vc = np.zeros((ck.n_layers, self.max_seq, ck.kv_dim), dtype=np.float32)
        self.kv_absmax = 0.0

    @staticmethod
    def _rmsnorm(x, g, eps):
        return g * (x / math.sqrt(float(np.mean(x * x)) + eps))

    def forward(self, token, pos):
        ck, t = self.ck, self.ck.t
        hs, kvd = ck.head_size, ck.kv_dim
        x = t["tok_emb"][token].astype(np.float32).copy()

        for l in range(ck.n_layers):
            xb = self._rmsnorm(x, t["rms_att"][l], self.eps)
            q = t["wq"][l] @ xb
            k = t["wk"][l] @ xb
            v = t["wv"][l] @ xb

            for vec, n in ((q, ck.dim), (k, kvd)):
                for i in range(0, n, 2):
                    j = (i % hs) // 2
                    freq = 1.0 / (10000.0 ** ((2 * j) / hs))
                    fcr, fci = math.cos(pos * freq), math.sin(pos * freq)
                    v0, v1 = vec[i], vec[i + 1]
                    vec[i] = v0 * fcr - v1 * fci
                    vec[i + 1] = v0 * fci + v1 * fcr

            self.kv_absmax = max(self.kv_absmax, float(np.abs(k).max()), float(np.abs(v).max()))
            self.kc[l, pos] = k
            self.vc[l, pos] = v

            xb = np.zeros(ck.dim, dtype=np.float32)
            kv_mul = ck.n_heads // ck.n_kv_heads
            for h in range(ck.n_heads):
                kvh = h // kv_mul
                qh = q[h * hs:(h + 1) * hs]
                ks = self.kc[l, :pos + 1, kvh * hs:(kvh + 1) * hs]
                sc = (ks @ qh) / math.sqrt(hs)
                sc = np.exp(sc - sc.max())
                sc /= sc.sum()
                vs = self.vc[l, :pos + 1, kvh * hs:(kvh + 1) * hs]
                xb[h * hs:(h + 1) * hs] = sc @ vs
            x = x + t["wo"][l] @ xb

            xb = self._rmsnorm(x, t["rms_ffn"][l], self.eps)
            h1 = t["w1"][l] @ xb
            h3 = t["w3"][l] @ xb
            h1 = h1 / (1.0 + np.exp(-h1)) * h3
            x = x + t["w2"][l] @ h1

        xb = self._rmsnorm(x, t["rms_final"], self.eps)
        wcls = t["tok_emb"] if self.ck.shared else t["wcls"]
        return wcls @ xb


# ---------------------------------------------------------------------------------------------
# FIXED REFERENCE — bit-exact mirror of examples/tinyllm/src/llm.cpp.
#
# Every operation below corresponds one-to-one with a line in llm.cpp: same order, same shifts,
# same saturation. numpy is used only where the intermediate provably fits int64 (asserted), so
# the result is identical to the C++ integer arithmetic, not merely close to it.
# ---------------------------------------------------------------------------------------------

class FixedModel:
    def __init__(self, qw, hdr, luts, rope, max_seq=None):
        self.q = qw                       # dict of (int8, int32-scales) tuples
        self.h = hdr
        self.exp_lut, self.silu_lut = luts
        self.rope_cos, self.rope_sin = rope
        self.group = hdr["group_size"]
        self.dim = hdr["dim"]
        self.hidden = hdr["hidden_dim"]
        self.n_layers = hdr["n_layers"]
        self.n_heads = hdr["n_heads"]
        self.n_kv_heads = hdr["n_kv_heads"]
        self.hs = hdr["head_size"]
        self.kv_dim = hdr["kv_dim"]
        self.vocab = hdr["vocab_size"]
        self.kv_shift = hdr["kv_shift"]
        self.eps = hdr["rms_eps_q16"]
        self.kv_mul = self.n_heads // self.n_kv_heads
        self.max_seq = max_seq or hdr["seq_len"]
        self.inv_dim = q_div(Q16_ONE, sat32(self.dim * 65536))
        self.attn_scale = q_rsqrt(sat32(self.hs * 65536))
        self.reset()

    def reset(self):
        self.kc = np.zeros((self.n_layers, self.n_kv_heads, self.max_seq, self.hs), dtype=np.int16)
        self.vc = np.zeros((self.n_layers, self.n_kv_heads, self.max_seq, self.hs), dtype=np.int16)

    # -- kernels -------------------------------------------------------------------------------
    def matvec(self, mat, xq, xs):
        w, s = mat
        rows, stride = w.shape
        groups = stride // self.group
        wg = w.reshape(rows, groups, self.group).astype(np.int64)
        xg = np.asarray(xq, dtype=np.int64).reshape(groups, self.group)
        ival = np.einsum("rgj,gj->rg", wg, xg)                     # exact: |ival| < 2^21
        assert np.abs(ival).max() < (1 << 40)
        term = (ival * s.astype(np.int64)) >> 14                   # |.| < 2^52
        acc = term.sum(axis=1)
        mag = np.abs(acc)
        if mag.max() < (1 << 31):
            # q_scale_q30's fast path, vectorized. It negates BEFORE shifting (truncation toward
            # zero), which is not the same as shifting a negative value (which floors) — getting
            # this backwards drifts one ULP per row and flips an argmax ~16 tokens later.
            p = (mag * np.int64(xs)) >> 30
            return [sat32(int(-v if a < 0 else v)) for v, a in zip(p, acc)]
        return [q_scale_q30(int(v), xs) for v in acc]

    def rmsnorm(self, x, gain):
        ss = int(np.sum(np.asarray(x, dtype=np.int64) ** 2))
        mean = sat32(((ss >> 16) * self.inv_dim) >> 16)
        r = q_rsqrt(mean + self.eps)
        return [q_mul(int(gain[i]), q_mul(r, int(x[i]))) for i in range(len(x))]

    def embed(self, token):
        w, s = self.q["tok_emb"]
        stride = w.shape[1]
        groups = stride // self.group
        row = w[token].astype(np.int64)
        out = []
        j = 0
        for g in range(groups):
            sc = int(s[token, g])
            for _ in range(self.group):
                if j >= self.dim:
                    break
                out.append(sat32((int(row[j]) * sc) >> 14))
                j += 1
        return out

    def rope(self, vec, n_heads, pos):
        half = self.hs // 2
        for h in range(n_heads):
            base = h * self.hs
            for j in range(half):
                v0, v1 = vec[base + 2 * j], vec[base + 2 * j + 1]
                fc, fs = int(self.rope_cos[pos, j]), int(self.rope_sin[pos, j])
                vec[base + 2 * j] = sat32((v0 * fc - v1 * fs) >> 15)
                vec[base + 2 * j + 1] = sat32((v0 * fs + v1 * fc) >> 15)

    def kv_pack(self, v):
        v >>= (16 - self.kv_shift)
        return max(-32768, min(32767, v))

    def attention(self, l, pos, xb):
        nt = pos + 1
        for kvh in range(self.n_kv_heads):
            kb = self.kc[l, kvh, :nt].astype(np.int64)
            vb = self.vc[l, kvh, :nt].astype(np.int64)
            for sub in range(self.kv_mul):
                h = kvh * self.kv_mul + sub
                qh = np.array(self.qv[h * self.hs:(h + 1) * self.hs], dtype=np.int64)
                dot = kb @ qh                                     # |.| < 2^47
                att = [q_mul(sat32(int(d) >> self.kv_shift), self.attn_scale) for d in dot]
                maxs = max(att)
                ex = []
                for a in att:
                    d = a - maxs
                    ex.append(0 if d <= -(16 << 16) else q_exp_neg(self.exp_lut, d))
                inv = q_div(Q16_ONE, sat32(sum(ex)))
                w15 = np.array([q_mul(e, inv) >> 1 for e in ex], dtype=np.int64)
                acc = w15 @ vb                                    # |.| < 2^31 by construction
                assert np.abs(acc).max() < (1 << 31)
                sh = self.kv_shift - 1
                for i in range(self.hs):
                    xb[h * self.hs + i] = int(acc[i]) >> sh

    def forward(self, token, pos, want_logits=True):
        x = self.embed(token)
        for l in range(self.n_layers):
            # --- attention block ---
            xb = self.rmsnorm(x, self.q["rms_att"][l])
            stride = self.q["wq"][l][0].shape[1]
            xq, xs = q_quantize(xb, self.dim, stride)
            self.qv = self.matvec(self.q["wq"][l], xq, xs)
            kbuf = self.matvec(self.q["wk"][l], xq, xs)
            vbuf = self.matvec(self.q["wv"][l], xq, xs)
            self.rope(self.qv, self.n_heads, pos)
            self.rope(kbuf, self.n_kv_heads, pos)
            for kvh in range(self.n_kv_heads):
                for i in range(self.hs):
                    self.kc[l, kvh, pos, i] = self.kv_pack(kbuf[kvh * self.hs + i])
                    self.vc[l, kvh, pos, i] = self.kv_pack(vbuf[kvh * self.hs + i])
            xb = [0] * self.dim
            self.attention(l, pos, xb)
            stride = self.q["wo"][l][0].shape[1]
            xq, xs = q_quantize(xb, self.dim, stride)
            xb2 = self.matvec(self.q["wo"][l], xq, xs)
            x = [sat32(x[i] + xb2[i]) for i in range(self.dim)]

            # --- feed-forward block ---
            xb = self.rmsnorm(x, self.q["rms_ffn"][l])
            stride = self.q["w1"][l][0].shape[1]
            xq, xs = q_quantize(xb, self.dim, stride)
            h1 = self.matvec(self.q["w1"][l], xq, xs)
            h3 = self.matvec(self.q["w3"][l], xq, xs)
            hb = [q_mul(q_silu(self.silu_lut, h1[i]), h3[i]) for i in range(self.hidden)]
            stride = self.q["w2"][l][0].shape[1]
            xq, xs = q_quantize(hb, self.hidden, stride)
            xb = self.matvec(self.q["w2"][l], xq, xs)
            x = [sat32(x[i] + xb[i]) for i in range(self.dim)]

        if not want_logits:
            return None
        xb = self.rmsnorm(x, self.q["rms_final"])
        stride = self.q["cls"][0].shape[1]
        xq, xs = q_quantize(xb, self.dim, stride)
        return self.matvec(self.q["cls"], xq, xs)


# ---------------------------------------------------------------------------------------------
# The bake
# ---------------------------------------------------------------------------------------------

def bake(ck, tok, group, out_path, rms_eps=1e-5, kv_shift=None, quiet=False):
    """Quantize, build every table, write the blob. Returns (blob_bytes, hdr, qw, luts, rope)."""
    t = ck.t
    exp_lut, silu_lut = build_exp_lut(), build_silu_lut()
    rope_cos, rope_sin = build_rope_tables(ck.seq_len, ck.head_size)

    if kv_shift is None:
        kv_shift = calibrate_kv_shift(ck, tok, quiet=quiet)

    hdr = dict(dim=ck.dim, hidden_dim=ck.hidden_dim, n_layers=ck.n_layers, n_heads=ck.n_heads,
               n_kv_heads=ck.n_kv_heads, vocab_size=ck.vocab_size, seq_len=ck.seq_len,
               group_size=group, head_size=ck.head_size, kv_dim=ck.kv_dim, kv_shift=kv_shift,
               flags=FLAG_SHARED_CLASSIFIER if ck.shared else 0,
               rms_eps_q16=max(1, int(round(rms_eps * 65536))),
               max_token_len=tok.max_len, bos_id=tok.bos, eos_id=tok.eos)

    w = BlobWriter()
    qw = {}
    report = []

    def q8(name, kind, mat, layer=NO_LAYER):
        q, s, (err, rel) = quantize_rows(mat, group)
        w.add_q8(name, kind, q, s, mat.shape[-1] if mat.ndim > 1 else mat.shape[0], layer)
        report.append((name, q.size, err, rel))
        return (q, s)

    qw["tok_emb"] = q8("tok_emb", K_TOK_EMB, t["tok_emb"])
    if ck.shared:
        qw["cls"] = qw["tok_emb"]
    else:
        qw["cls"] = q8("wcls", K_WCLS, t["wcls"])

    for nm, kind in (("wq", K_WQ), ("wk", K_WK), ("wv", K_WV), ("wo", K_WO),
                     ("w1", K_W1), ("w2", K_W2), ("w3", K_W3)):
        qw[nm] = [q8(f"{nm}.{l}", kind, t[nm][l], l) for l in range(ck.n_layers)]

    # RMSNorm gains, RoPE tables and the tokenizer stay in higher precision (Q16.16 / Q15) —
    # they are a rounding error's worth of bytes and quantizing them costs real quality.
    def i32_gain(v):
        return np.clip(np.rint(np.asarray(v, dtype=np.float64) * 65536.0),
                       -2147483648, 2147483647).astype(np.int32)

    qw["rms_att"] = [i32_gain(t["rms_att"][l]) for l in range(ck.n_layers)]
    qw["rms_ffn"] = [i32_gain(t["rms_ffn"][l]) for l in range(ck.n_layers)]
    qw["rms_final"] = i32_gain(t["rms_final"])
    for l in range(ck.n_layers):
        w.add_vec(f"rms_att.{l}", K_RMS_ATT, qw["rms_att"][l], DT_I32, l)
        w.add_vec(f"rms_ffn.{l}", K_RMS_FFN, qw["rms_ffn"][l], DT_I32, l)
    w.add_vec("rms_final", K_RMS_FINAL, qw["rms_final"], DT_I32)

    w.add_vec("rope_cos", K_ROPE_COS, rope_cos, DT_I16)
    w.add_vec("rope_sin", K_ROPE_SIN, rope_sin, DT_I16)
    w.add_vec("exp_lut", K_EXP_LUT, exp_lut, DT_I32)
    w.add_vec("silu_lut", K_SILU_LUT, silu_lut, DT_I32)

    joined = b"".join(tok.pieces)
    index = np.cumsum([0] + [len(p) for p in tok.pieces]).astype(np.uint32)
    w.add_bytes("tok_bytes", K_TOK_BYTES, joined)
    w.add_vec("tok_index", K_TOK_INDEX, index, DT_U32)
    w.add_vec("tok_sorted", K_TOK_SORTED, np.array(tok.sorted_ids, dtype=np.uint16), DT_U16)
    w.add_vec("tok_score", K_TOK_SCORE, i32_gain(tok.scores), DT_I32)

    blob = w.write(out_path, hdr)

    if not quiet:
        print(f"  model: {ck.describe()}")
        print(f"  quantization: int8, group={group}, per-group Q30 scales; "
              f"kv cache int16 Q{kv_shift}")
        print(f"  {'tensor':<12} {'elems':>9} {'max abs err':>12} {'rel rms err':>12}")
        for nm, n, err, rel in report:
            print(f"  {nm:<12} {n:>9} {err:>12.6f} {rel:>11.3%}")
        size_breakdown(blob, w)
    return blob, hdr, qw, (exp_lut, silu_lut), (rope_cos, rope_sin)


def size_breakdown(blob, writer):
    by_kind = {}
    for e in writer.entries:
        n = len(e["payload"]) + (len(e["scales"]) if e["scales"] else 0)
        key = e["name"].split(".")[0]
        by_kind[key] = by_kind.get(key, 0) + n
    print(f"  blob: {len(blob)} bytes ({len(blob)/1024:.1f} KiB)")
    for k in sorted(by_kind, key=lambda k: -by_kind[k]):
        print(f"    {k:<12} {by_kind[k]:>9} B  {by_kind[k]*100.0/len(blob):5.1f}%")


CALIB_TEXT = "Once upon a time there was a little girl who loved to play in the garden."


def calibrate_kv_shift(ck, tok, quiet=False):
    """Run the float reference over a calibration prompt to see how big K/V actually get, then
    pick the int16 Q shift from that instead of guessing a constant."""
    fm = FloatModel(ck)
    ids = tok.encode(CALIB_TEXT)[:min(64, ck.seq_len)]
    for pos, tid in enumerate(ids):
        fm.forward(tid, pos)
    sh = pick_kv_shift(fm.kv_absmax)
    if not quiet:
        print(f"  kv calibration: max|k|,|v| = {fm.kv_absmax:.3f} over {len(ids)} tokens "
              f"-> kv_shift = {sh} (int16 headroom {32767/(fm.kv_absmax*(1<<sh)):.1f}x)")
    return sh


# ---------------------------------------------------------------------------------------------
# Golden tokens + evaluation
# ---------------------------------------------------------------------------------------------

class FixedSampler:
    """Mirror of sample() in llm.cpp: fixed-point temperature, softmax through the ROM exp table,
    top-p nucleus filtering with a partial selection sort, and a seeded xorshift32. Reproduces the
    device's choices exactly, which is what makes the sampled golden worth asserting."""

    def __init__(self, seed, temp_q16, topp_q16):
        self.rng = seed & 0xFFFFFFFF or 1
        self.temp = temp_q16
        self.topp = topp_q16

    def _next(self):
        x = self.rng
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        self.rng = x or 1
        return self.rng

    def pick(self, logits, exp_lut):
        n = len(logits)
        p = [int(v) for v in logits]
        if self.temp != Q16_ONE:
            inv = q_div(Q16_ONE, self.temp)
            p = [q_mul(v, inv) for v in p]
        maxl = max(p)
        total = 0
        for i in range(n):
            d = p[i] - maxl
            p[i] = 0 if d <= -(16 << 16) else q_exp_neg(exp_lut, d)
            total += p[i]
        norm = q_div(Q16_ONE, sat32(total))
        p = [q_mul(v, norm) for v in p]

        cutoff = q_div(Q16_ONE - self.topp, sat32((n - 1) * 65536)) if n > 1 else 0
        order = [i for i in range(n) if p[i] >= cutoff] or list(range(n))
        ncand = len(order)
        cum, kept = 0, 0
        while kept < ncand:
            bi = kept
            for j in range(kept + 1, ncand):
                if p[order[j]] > p[order[bi]]:
                    bi = j
            order[kept], order[bi] = order[bi], order[kept]
            cum += p[order[kept]]
            kept += 1
            if cum >= self.topp:
                break
        r = (self._next() * cum) >> 32
        acc = 0
        for j in range(kept):
            acc += p[order[j]]
            if r < acc:
                return order[j]
        return order[kept - 1] if kept else 0


def generate_sampled(fx, prompt_ids, steps, eos, sampler, exp_lut):
    fx.reset()
    out, logits = [], None
    for pos, tid in enumerate(prompt_ids):
        logits = fx.forward(tid, pos, want_logits=(pos == len(prompt_ids) - 1))
    pos = len(prompt_ids)
    for _ in range(steps):
        if logits is None or pos >= fx.max_seq:
            break
        nxt = sampler.pick(logits, exp_lut)
        out.append(nxt)
        if nxt == eos:
            break
        logits = fx.forward(nxt, pos)
        pos += 1
    return out


def generate_fixed(fx, prompt_ids, steps, eos):
    fx.reset()
    out = []
    logits = None
    for pos, tid in enumerate(prompt_ids):
        logits = fx.forward(tid, pos, want_logits=(pos == len(prompt_ids) - 1))
    pos = len(prompt_ids)
    for _ in range(steps):
        if logits is None or pos >= fx.max_seq:
            break
        nxt = int(np.argmax(np.array(logits, dtype=np.int64)))
        out.append(nxt)
        if nxt == eos:
            break
        logits = fx.forward(nxt, pos)
        pos += 1
    return out


def generate_float(fm, prompt_ids, steps, eos):
    fm.reset()
    out = []
    logits = None
    for pos, tid in enumerate(prompt_ids):
        logits = fm.forward(tid, pos)
    pos = len(prompt_ids)
    for _ in range(steps):
        if logits is None or pos >= fm.max_seq:
            break
        nxt = int(np.argmax(logits))
        out.append(nxt)
        if nxt == eos:
            break
        logits = fm.forward(nxt, pos)
        pos += 1
    return out


SAMPLER_TEMP_Q16 = 58982   # 0.90
SAMPLER_TOPP_Q16 = 58982   # 0.90


def write_golden(path, prompt, prompt_ids, fixed_ids, float_ids, sampled_ids,
                 seed, model_note, blob):
    agree = 0
    for a, b in zip(fixed_ids, float_ids):
        if a != b:
            break
        agree += 1
    with open(path, "w", encoding="utf-8") as f:
        f.write("# tinyllm golden tokens -- generated by examples/tinyllm/tools/export_model.py\n")
        f.write("# DO NOT EDIT BY HAND. `fixed_tokens` is what the C++ core must reproduce\n")
        f.write("# exactly; `float_tokens` is the float reference, recorded so quantization\n")
        f.write("# drift is measured rather than assumed. blob_crc/blob_size identify the model\n")
        f.write("# these tokens belong to -- the suite checks them FIRST, so a changed fixture\n")
        f.write("# reports as a changed fixture instead of as a mysterious token mismatch.\n")
        f.write(f"# model: {model_note}\n")
        f.write(f"# float/fixed agree on the first {agree} of {len(fixed_ids)} tokens\n")
        f.write("version 1\n")
        f.write(f"seed {seed}\n")
        f.write(f"temperature_q16 {SAMPLER_TEMP_Q16}\n")
        f.write(f"topp_q16 {SAMPLER_TOPP_Q16}\n")
        f.write(f"blob_size {len(blob)}\n")
        f.write(f"blob_crc {crc32_of(blob[52:])}\n")
        f.write(f"prompt {prompt}\n")
        f.write("prompt_tokens " + " ".join(str(i) for i in prompt_ids) + "\n")
        f.write("fixed_tokens " + " ".join(str(i) for i in fixed_ids) + "\n")
        f.write("float_tokens " + " ".join(str(i) for i in float_ids) + "\n")
        f.write("sampled_tokens " + " ".join(str(i) for i in sampled_ids) + "\n")
    return agree


EVAL_TEXT = (
    "Once upon a time, there was a little boy named Tim. Tim had a small red ball. "
    "He liked to play with his ball every day. One day, the ball rolled into the tall grass. "
    "Tim was sad because he could not find it. His mom came outside and helped him look. "
    "They found the ball under a big tree and Tim was very happy again."
)


def perplexity(logit_fn, ids):
    """Teacher-forced perplexity over `ids`, scored with a numerically stable log-softmax."""
    total, n = 0.0, 0
    for pos in range(len(ids) - 1):
        lg = np.asarray(logit_fn(ids[pos], pos), dtype=np.float64)
        lg = lg - lg.max()
        total -= lg[ids[pos + 1]] - math.log(np.exp(lg).sum())
        n += 1
    return math.exp(total / max(1, n))


# ---------------------------------------------------------------------------------------------
# The tiny fixture model
# ---------------------------------------------------------------------------------------------

def build_fixture_checkpoint(seed=7):
    """A structurally correct, deliberately tiny transformer with random weights: ~11 K
    parameters, so the committed fixture stays small and the suites stay fast."""
    rng = np.random.default_rng(seed)
    dim, hidden, n_layers, n_heads, n_kv_heads, seq_len = 16, 32, 2, 4, 2, 32
    tok = build_fixture_tokenizer()
    vocab = len(tok.pieces)
    hs = dim // n_heads
    kvd = hs * n_kv_heads

    def n(*shape, s=0.5):
        return (rng.standard_normal(shape) * s).astype(np.float32)

    t = dict(
        tok_emb=n(vocab, dim), rms_att=np.abs(n(n_layers, dim, s=0.3)) + 0.7,
        wq=n(n_layers, dim, dim, s=0.35), wk=n(n_layers, kvd, dim, s=0.35),
        wv=n(n_layers, kvd, dim, s=0.35), wo=n(n_layers, dim, dim, s=0.35),
        rms_ffn=np.abs(n(n_layers, dim, s=0.3)) + 0.7,
        w1=n(n_layers, hidden, dim, s=0.3), w2=n(n_layers, dim, hidden, s=0.3),
        w3=n(n_layers, hidden, dim, s=0.3),
        rms_final=np.abs(n(dim, s=0.3)) + 0.7)
    cfg = dict(dim=dim, hidden_dim=hidden, n_layers=n_layers, n_heads=n_heads,
               n_kv_heads=n_kv_heads, vocab_size=vocab, seq_len=seq_len)
    return Checkpoint(cfg, t, shared=True), tok


# ---------------------------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------------------------

def cmd_export(a):
    ck = load_checkpoint(a.checkpoint)
    tok = load_tokenizer(a.tokenizer)
    if len(tok.pieces) != ck.vocab_size:
        raise SystemExit(f"tokenizer has {len(tok.pieces)} pieces but the checkpoint's vocab "
                         f"is {ck.vocab_size}")
    print(f"exporting {a.checkpoint} -> {a.out}")
    blob, hdr, qw, luts, rope = bake(ck, tok, a.group_size, a.out, kv_shift=a.kv_shift)
    if a.golden:
        emit_golden(a, ck, tok, hdr, qw, luts, rope, blob, os.path.basename(a.checkpoint))
    return 0


def cmd_golden(a):
    """Regenerate a golden file from an EXISTING .phxllm. This is the path the committed test
    golden takes: examples/tinyllm/src/llm_build.h builds the fixture blob in C++ (so the repo
    tracks no binaries), `make tinyllm-fixture` dumps it, and this reads those exact bytes."""
    blob, hdr, qw, luts, rope, tok = load_blob(a.blob)
    print(f"golden from {a.blob}: dim={hdr['dim']} layers={hdr['n_layers']} "
          f"vocab={hdr['vocab_size']} group={hdr['group_size']} kv_shift={hdr['kv_shift']} "
          f"({len(blob)} bytes, crc {crc32_of(blob[52:]):#010x})")
    ck = dequantized_checkpoint(hdr, qw)
    emit_golden(a, ck, tok, hdr, qw, luts, rope, blob,
                f"{os.path.basename(a.blob)} ({len(blob)} bytes)")
    return 0


def cmd_fixture(a):
    ck, tok = build_fixture_checkpoint()
    print(f"building the tiny random fixture -> {a.out}")
    blob, hdr, qw, luts, rope = bake(ck, tok, a.group_size, a.out, kv_shift=a.kv_shift)
    if a.golden:
        emit_golden(a, ck, tok, hdr, qw, luts, rope, blob, "random fixture (seed 7)")
    return 0


def emit_golden(a, ck, tok, hdr, qw, luts, rope, blob, note):
    prompt_ids = tok.encode(a.prompt)
    steps = min(a.steps, ck.seq_len - len(prompt_ids))
    if steps <= 0:
        raise SystemExit("the prompt fills the whole context; nothing to generate")
    print(f"  golden: prompt {prompt_ids} + {steps} greedy tokens")

    fx = FixedModel(qw, hdr, luts, rope)
    fixed_ids = generate_fixed(fx, prompt_ids, steps, tok.eos)
    fm = FloatModel(ck)
    float_ids = generate_float(fm, prompt_ids, steps, tok.eos)
    sampler = FixedSampler(a.seed, SAMPLER_TEMP_Q16, SAMPLER_TOPP_Q16)
    sampled_ids = generate_sampled(fx, prompt_ids, steps, tok.eos, sampler, luts[0])

    agree = write_golden(a.golden, a.prompt, prompt_ids, fixed_ids, float_ids, sampled_ids,
                         a.seed, note, blob)
    print(f"  golden -> {a.golden}")
    print(f"    greedy(fixed): {render(tok, prompt_ids, fixed_ids)!r}")
    print(f"    greedy(float): {render(tok, prompt_ids, float_ids)!r}")
    print(f"    top-p  (fixed): {render(tok, prompt_ids, sampled_ids)!r}")
    print(f"    float/fixed agree on the first {agree}/{len(fixed_ids)} tokens")


def render(tok, prompt_ids, ids):
    out, prev = b"", (prompt_ids[-1] if prompt_ids else tok.bos)
    for i in ids:
        out += tok.decode(prev, i)
        prev = i
    return out.decode("utf-8", "replace")


def cmd_eval(a):
    ck = load_checkpoint(a.checkpoint)
    tok = load_tokenizer(a.tokenizer)
    tmp = a.out or os.devnull
    blob, hdr, qw, luts, rope = bake(ck, tok, a.group_size, tmp, kv_shift=a.kv_shift, quiet=True)
    ids = tok.encode(EVAL_TEXT)[:min(a.limit, ck.seq_len)]
    print(f"held-out sample: {len(ids)} tokens")
    fm = FloatModel(ck)
    ppl_f = perplexity(lambda t, p: fm.forward(t, p), ids)
    fx = FixedModel(qw, hdr, luts, rope)
    # the fixed core returns Q16.16 logits; scale them back to real units to score
    ppl_q = perplexity(lambda t, p: np.asarray(fx.forward(t, p), dtype=np.float64) / 65536.0, ids)
    print(f"  float32 perplexity      {ppl_f:8.4f}")
    print(f"  int8 fixed perplexity   {ppl_q:8.4f}")
    print(f"  delta                   {ppl_q - ppl_f:+8.4f}  ({(ppl_q/ppl_f - 1)*100:+.2f}%)")
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    def common(sp, need_ck, need_out=True):
        if need_ck:
            sp.add_argument("--checkpoint", required=True,
                            help="llama2.c legacy .bin, or a PyTorch .pt/.pth state_dict")
            sp.add_argument("--tokenizer", required=True, help="llama2.c tokenizer .bin")
        sp.add_argument("--out", required=need_out, help="output .phxllm path")
        sp.add_argument("--group-size", type=int, default=32,
                        help="int8 quantization group (power of two, >= 4; default 32)")
        sp.add_argument("--kv-shift", type=int, default=None,
                        help="override the calibrated KV cache Q shift")
        sp.add_argument("--golden", help="also write a golden-token file here")
        sp.add_argument("--prompt", default="Once upon a time",
                        help="prompt for the golden file")
        sp.add_argument("--steps", type=int, default=24, help="golden tokens to generate")
        sp.add_argument("--seed", type=int, default=0x2545F491, help="sampler seed to record")

    sp = sub.add_parser("export", help="bake a checkpoint into a .phxllm blob")
    common(sp, True)
    sp.set_defaults(fn=cmd_export)

    sp = sub.add_parser("fixture", help="build the tiny random test fixture")
    common(sp, False)
    sp.set_defaults(fn=cmd_fixture)

    sp = sub.add_parser("eval", help="report the int8-vs-float perplexity delta")
    common(sp, True, need_out=False)
    sp.add_argument("--limit", type=int, default=96, help="tokens of the sample to score")
    sp.set_defaults(fn=cmd_eval)

    sp = sub.add_parser("golden", help="write a golden-token file for an existing .phxllm")
    sp.add_argument("--blob", required=True, help="the .phxllm to read")
    sp.add_argument("--golden", required=True, help="output golden path")
    sp.add_argument("--prompt", default="Once upon a time")
    sp.add_argument("--steps", type=int, default=24)
    sp.add_argument("--seed", type=int, default=0x2545F491)
    sp.set_defaults(fn=cmd_golden, group_size=32)

    a = p.parse_args(argv)
    if a.group_size < 4 or a.group_size > 256 or (a.group_size & (a.group_size - 1)):
        raise SystemExit("--group-size must be a power of two in [4, 256]")
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
