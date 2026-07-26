#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.  The
# ASF licenses this file to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance with the
# License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations
# under the License.

"""Convert a HuggingFace LLaMA-family checkpoint into the .nylm container
consumed by app/llm on the KICKPI-K7.

Supported architectures: LlamaForCausalLM, Qwen2ForCausalLM (and anything
else that shares the same block layout and tensor names).

Usage:
    pip install numpy safetensors
    python3 hf_to_nylm.py <hf_model_dir> <out.nylm> [--ctx 2048]

The projection weights and the embedding table are quantised to q4_0
(32 values per block, one fp16 scale per block, 4.5 bit/weight).  The
RMSNorm weights and the Q/K/V biases stay fp32 because they are tiny and
because the runtime relies on that.
"""

import argparse
import json
import os
import struct
import sys

import numpy as np

MODEL_MAGIC = 0x4D4C594E          # "NYLM"
TOKENIZER_MAGIC = 0x4B54594E      # "NYTK"
MODEL_VERSION = 1
HEADER_BYTES = 128
TENSOR_ENTRY_BYTES = 64
NAME_MAX = 32
PAYLOAD_ALIGN = 64

Q4_BLOCK = 32
Q4_BLOCK_BYTES = 18

DTYPE_F32 = 0
DTYPE_F16 = 1
DTYPE_Q4_0 = 2

FLAG_TIE_EMBED = 1 << 0
FLAG_QKV_BIAS = 1 << 1
FLAG_ROPE_NEOX = 1 << 2

TOKFLAG_ADD_BOS = 1 << 0


def quantize_q4_0(mat):
    """Quantise a 2-D float32 array row-wise into the q4_0 block layout.

    Layout per block: fp16 scale, then 16 bytes where the low nibble of
    byte j is value j and the high nibble is value j + 16.  This matches
    llama.cpp's q4_0 so the numerics are a known quantity.
    """
    rows, cols = mat.shape
    assert cols % Q4_BLOCK == 0, "row length must be a multiple of 32"

    x = mat.astype(np.float32).reshape(rows * cols // Q4_BLOCK, Q4_BLOCK)

    # Scale is derived from the value with the largest magnitude, keeping
    # its sign, so that value maps exactly onto nibble 0.
    imax = np.argmax(np.abs(x), axis=1)
    vmax = x[np.arange(x.shape[0]), imax]
    d = (vmax / -8.0).astype(np.float32)
    inv = np.where(d != 0.0, 1.0 / np.where(d == 0.0, 1.0, d), 0.0)

    q = np.clip((x * inv[:, None] + 8.5).astype(np.int32), 0, 15)
    q = q.astype(np.uint8)

    packed = (q[:, :16] | (q[:, 16:] << 4)).astype(np.uint8)

    out = np.empty((x.shape[0], Q4_BLOCK_BYTES), dtype=np.uint8)
    out[:, 0:2] = d.astype(np.float16).view(np.uint8).reshape(-1, 2)
    out[:, 2:] = packed
    return out.tobytes()


def bytes_to_unicode():
    """GPT-2 reversible byte <-> unicode table (same code as HF)."""
    bs = (list(range(ord("!"), ord("~") + 1)) +
          list(range(ord("\xa1"), ord("\xac") + 1)) +
          list(range(ord("\xae"), ord("\xff") + 1)))
    cs = bs[:]
    n = 0
    for b in range(256):
        if b not in bs:
            bs.append(b)
            cs.append(256 + n)
            n += 1
    return dict(zip(bs, [chr(c) for c in cs]))


class TensorTable:
    """Collects tensor payloads and emits the directory + data section."""

    def __init__(self):
        self.entries = []

    def add(self, name, rows, cols, dtype, payload):
        assert len(name) < NAME_MAX, "tensor name too long: " + name
        self.entries.append([name, int(rows), int(cols), dtype, payload])

    def add_f32(self, name, arr):
        arr = np.ascontiguousarray(arr, dtype=np.float32)
        if arr.ndim == 1:
            rows, cols = 1, arr.shape[0]
        else:
            rows, cols = arr.shape
        self.add(name, rows, cols, DTYPE_F32, arr.tobytes())

    def add_q4(self, name, arr):
        arr = np.ascontiguousarray(arr, dtype=np.float32)
        rows, cols = arr.shape
        if cols % Q4_BLOCK != 0:
            print("  %s: cols %d not a multiple of %d, keeping fp32" %
                  (name, cols, Q4_BLOCK))
            self.add(name, rows, cols, DTYPE_F32, arr.tobytes())
            return
        self.add(name, rows, cols, DTYPE_Q4_0, quantize_q4_0(arr))


def build_tokenizer_blob(model_dir, vocab_size, add_bos):
    tok_path = os.path.join(model_dir, "tokenizer.json")
    if not os.path.exists(tok_path):
        print("warning: no tokenizer.json, the model will not be usable "
              "for generation")
        return b""

    with open(tok_path, "r", encoding="utf-8") as f:
        tj = json.load(f)

    vocab = tj["model"]["vocab"]
    merges = tj["model"].get("merges", [])
    added = {t["content"]: t["id"] for t in tj.get("added_tokens", [])}

    b2u = bytes_to_unicode()
    u2b = {v: k for k, v in b2u.items()}

    def decode_piece(piece):
        try:
            return bytes(u2b[ch] for ch in piece)
        except KeyError:
            # Not in the byte-level alphabet: a special/added token.
            return piece.encode("utf-8")

    table = [b""] * vocab_size
    for piece, idx in vocab.items():
        if idx < vocab_size:
            table[idx] = decode_piece(piece)
    for piece, idx in added.items():
        if idx < vocab_size:
            table[idx] = piece.encode("utf-8")

    # Any id the tokenizer never defines must still be present so decode()
    # cannot run off the end; an empty string renders as nothing.
    missing = sum(1 for t in table if t == b"")
    if missing:
        print("  %d vocabulary slots are unused padding" % missing)

    piece_to_id = {}
    for piece, idx in vocab.items():
        piece_to_id[piece] = idx

    merge_recs = []
    for m in merges:
        if isinstance(m, str):
            parts = m.split(" ")
        else:
            parts = list(m)
        if len(parts) != 2:
            continue
        a, b = parts
        ia = piece_to_id.get(a)
        ib = piece_to_id.get(b)
        iab = piece_to_id.get(a + b)
        if ia is None or ib is None or iab is None:
            continue
        if ia >= vocab_size or ib >= vocab_size or iab >= vocab_size:
            continue
        merge_recs.append((ia, ib, iab))

    flags = TOKFLAG_ADD_BOS if add_bos else 0
    out = bytearray()
    out += struct.pack("<IIII", TOKENIZER_MAGIC, vocab_size,
                       len(merge_recs), flags)
    for piece in table:
        out += struct.pack("<H", len(piece))
        out += piece
    for ia, ib, iab in merge_recs:
        out += struct.pack("<III", ia, ib, iab)

    print("  tokenizer: %d tokens, %d merges, %d bytes" %
          (vocab_size, len(merge_recs), len(out)))
    return bytes(out)


def load_state_dict(model_dir):
    """Load every tensor of the checkpoint as a numpy array."""
    index = os.path.join(model_dir, "model.safetensors.index.json")
    shards = []
    if os.path.exists(index):
        with open(index, "r", encoding="utf-8") as f:
            shards = sorted(set(json.load(f)["weight_map"].values()))
    elif os.path.exists(os.path.join(model_dir, "model.safetensors")):
        shards = ["model.safetensors"]

    state = {}
    if shards:
        from safetensors.numpy import load_file
        for shard in shards:
            state.update(load_file(os.path.join(model_dir, shard)))
        return state

    import torch
    bin_path = os.path.join(model_dir, "pytorch_model.bin")
    sd = torch.load(bin_path, map_location="cpu")
    for k, v in sd.items():
        state[k] = v.to(torch.float32).numpy()
    return state


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("model_dir")
    ap.add_argument("out")
    ap.add_argument("--ctx", type=int, default=2048,
                    help="context window baked into the header")
    ap.add_argument("--add-bos", action="store_true",
                    help="prepend BOS when encoding (LLaMA yes, Qwen no)")
    args = ap.parse_args()

    with open(os.path.join(args.model_dir, "config.json"),
              "r", encoding="utf-8") as f:
        cfg = json.load(f)

    hidden = cfg["hidden_size"]
    ffn = cfg["intermediate_size"]
    n_layers = cfg["num_hidden_layers"]
    n_heads = cfg["num_attention_heads"]
    n_kv = cfg.get("num_key_value_heads", n_heads)
    head_dim = cfg.get("head_dim", hidden // n_heads)
    vocab = cfg["vocab_size"]
    ctx = min(args.ctx, cfg.get("max_position_embeddings", args.ctx))
    rope_theta = float(cfg.get("rope_theta", 10000.0))
    rms_eps = float(cfg.get("rms_norm_eps", 1e-6))
    tie = bool(cfg.get("tie_word_embeddings", False))

    print("config: hidden %d ffn %d layers %d heads %d/%d head_dim %d "
          "vocab %d ctx %d" % (hidden, ffn, n_layers, n_heads, n_kv,
                               head_dim, vocab, ctx))

    state = load_state_dict(args.model_dir)
    has_bias = "model.layers.0.self_attn.q_proj.bias" in state

    flags = FLAG_ROPE_NEOX          # HF rotate_half pairing
    if tie:
        flags |= FLAG_TIE_EMBED
    if has_bias:
        flags |= FLAG_QKV_BIAS

    tt = TensorTable()

    def get(name):
        if name not in state:
            raise KeyError("checkpoint is missing " + name)
        return np.asarray(state[name], dtype=np.float32)

    print("quantising...")
    tt.add_q4("tok_emb", get("model.embed_tokens.weight"))
    tt.add_f32("out_norm", get("model.norm.weight"))
    if not tie:
        tt.add_q4("output", get("lm_head.weight"))

    for i in range(n_layers):
        p = "model.layers.%d." % i
        tt.add_f32("blk.%d.attn_norm" % i, get(p + "input_layernorm.weight"))
        tt.add_q4("blk.%d.attn_q" % i, get(p + "self_attn.q_proj.weight"))
        tt.add_q4("blk.%d.attn_k" % i, get(p + "self_attn.k_proj.weight"))
        tt.add_q4("blk.%d.attn_v" % i, get(p + "self_attn.v_proj.weight"))
        tt.add_q4("blk.%d.attn_o" % i, get(p + "self_attn.o_proj.weight"))
        if has_bias:
            tt.add_f32("blk.%d.attn_q_b" % i,
                       get(p + "self_attn.q_proj.bias"))
            tt.add_f32("blk.%d.attn_k_b" % i,
                       get(p + "self_attn.k_proj.bias"))
            tt.add_f32("blk.%d.attn_v_b" % i,
                       get(p + "self_attn.v_proj.bias"))
        tt.add_f32("blk.%d.ffn_norm" % i,
                   get(p + "post_attention_layernorm.weight"))
        tt.add_q4("blk.%d.ffn_gate" % i, get(p + "mlp.gate_proj.weight"))
        tt.add_q4("blk.%d.ffn_up" % i, get(p + "mlp.up_proj.weight"))
        tt.add_q4("blk.%d.ffn_down" % i, get(p + "mlp.down_proj.weight"))
        if (i + 1) % 4 == 0:
            print("  layer %d/%d" % (i + 1, n_layers))

    tok_blob = build_tokenizer_blob(args.model_dir, vocab, args.add_bos)

    gen_cfg = {}
    gen_path = os.path.join(args.model_dir, "generation_config.json")
    if os.path.exists(gen_path):
        with open(gen_path, "r", encoding="utf-8") as f:
            gen_cfg = json.load(f)
    bos = gen_cfg.get("bos_token_id", cfg.get("bos_token_id", 0))
    eos = gen_cfg.get("eos_token_id", cfg.get("eos_token_id", 0))
    if isinstance(bos, list):
        bos = bos[0]
    if isinstance(eos, list):
        eos = eos[0]

    # Lay the file out: header, directory, tokenizer, aligned payloads.
    table_off = HEADER_BYTES
    table_bytes = len(tt.entries) * TENSOR_ENTRY_BYTES
    tok_off = table_off + table_bytes
    cursor = tok_off + len(tok_blob)

    offsets = []
    for entry in tt.entries:
        cursor = (cursor + PAYLOAD_ALIGN - 1) & ~(PAYLOAD_ALIGN - 1)
        offsets.append(cursor)
        cursor += len(entry[4])

    header = struct.pack(
        "<IIII" "iiiiiiii" "ff" "IIIII" "II" "44x",
        MODEL_MAGIC, MODEL_VERSION, 0, flags,
        hidden, ffn, n_layers, n_heads, n_kv, head_dim, vocab, ctx,
        rope_theta, rms_eps,
        len(tt.entries), table_off, table_bytes, tok_off, len(tok_blob),
        int(bos), int(eos))
    assert len(header) == HEADER_BYTES, len(header)

    with open(args.out, "wb") as f:
        f.write(header)
        for entry, off in zip(tt.entries, offsets):
            name, rows, cols, dtype, payload = entry
            rec = struct.pack("<32sIIIIQQ", name.encode("ascii"), dtype,
                              rows, cols, 0, off, len(payload))
            assert len(rec) == TENSOR_ENTRY_BYTES, len(rec)
            f.write(rec)
        f.write(tok_blob)
        for entry, off in zip(tt.entries, offsets):
            f.seek(off)
            f.write(entry[4])

    size = os.path.getsize(args.out)
    print("wrote %s: %.1f MiB, %d tensors" %
          (args.out, size / (1024.0 * 1024.0), len(tt.entries)))
    print("kv-cache at ctx=%d will need %.1f MiB of RAM" %
          (ctx, 2.0 * n_layers * ctx * n_kv * head_dim * 4 /
           (1024.0 * 1024.0)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
