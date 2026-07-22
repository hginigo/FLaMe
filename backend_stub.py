#!/usr/bin/env python3
"""Minimal reference backend implementing protocol.txt over stdin/stdout.

Not a real model: each replica is a small numpy vector nudged towards a
per-node target, letting `loss` genuinely decrease under training and
genuinely drop further under aggregation, without pulling in a real ML
framework. Swap `train`/`aggregate` for real model code and this file stays
a valid backend for blokek.
"""
import sys
import json
import struct
import numpy as np

DIM = 8


def read_msg():
    line = sys.stdin.buffer.readline()
    if not line:
        return None
    msg = json.loads(line)
    nbytes = msg.get("nbytes", 0)
    payload = sys.stdin.buffer.read(nbytes) if nbytes else b""
    return msg, payload


def write_msg(header, payload=b""):
    header = dict(header)
    header["nbytes"] = len(payload)
    sys.stdout.buffer.write((json.dumps(header) + "\n").encode())
    sys.stdout.buffer.write(payload)
    sys.stdout.buffer.flush()


def pack(vec):
    return struct.pack(f"{len(vec)}d", *vec)


def unpack(buf):
    n = len(buf) // 8
    return np.array(struct.unpack(f"{n}d", buf))


targets = {}


def main():
    while True:
        msg = read_msg()
        if msg is None:
            return
        header, payload = msg
        op = header["op"]

        if op == "shutdown":
            return

        elif op == "init":
            node = header["node"]
            rng = np.random.default_rng(header.get("seed", 0) + node)
            targets[node] = rng.normal(0, 1, DIM)
            w = rng.normal(0, 0.1, DIM)
            write_msg({"op": "ready", "node": node}, pack(w))

        elif op == "train":
            node = header["node"]
            w = unpack(payload)
            target = targets[node]
            lr = 0.2
            for _ in range(header.get("epochs", 1)):
                w = w - lr * (w - target)
            loss = float(np.mean((w - target) ** 2))
            write_msg({
                "op": "trained", "node": node,
                "version": header["version"] + 1,
                "sim_ms": 400, "loss": loss, "acc": max(0.0, 1 - loss),
            }, pack(w))

        elif op == "aggregate":
            node = header["node"]
            count = header["count"]
            blob_len = len(payload) // count if count else 0
            vecs = [unpack(payload[i*blob_len:(i+1)*blob_len]) for i in range(count)]
            w = np.mean(vecs, axis=0)
            loss = float(np.mean((w - targets[node]) ** 2))
            write_msg({"op": "aggregated", "node": node, "sim_ms": 50}, pack(w))
            _ = loss

        else:
            sys.exit(f"unknown op {op}")


if __name__ == "__main__":
    main()
