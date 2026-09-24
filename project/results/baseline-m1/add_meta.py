#!/usr/bin/env python3
"""add_meta.py — augment baseline summary JSONs with common meta fields.

usage: add_meta.py <summary.json> [more.json ...]
Adds (idempotently) a "meta" object to each JSON file:
  {"kernel","bzimage_sha256","runs","date","qemu"}
If the root is a list, wraps as {"meta":..., "results":[...]} is NOT done —
instead each element... (lists are left untouched; meta is added under key
"meta" only for dict roots). mmbench_summary.json is a list → it is converted
to {"meta":..., "results":[...]}? NO — keep the runner-produced list format
byte-stable. For lists, a sibling file <name>_meta.json is written instead.
"""
import hashlib, json, os, subprocess, sys, datetime

KERNEL = "6.18.32-g68974e235117"
BZSHA = "038ed5b38e76947b768006cb3171c231ea4acd5a3cf3515cd2560273776c2be9"
QEMU = "8vCPU/4G/KVM"
DATE = "2026-09-13"

def meta(runs):
    return {
        "kernel": KERNEL,
        "bzimage_sha256": BZSHA,
        "runs": runs,
        "date": DATE,
        "qemu": QEMU,
    }

for path in sys.argv[1:]:
    with open(path) as f:
        data = json.load(f)
    if isinstance(data, dict):
        runs = data.get("runs") if isinstance(data.get("runs"), int) else (
            len(data.get("runs")) if isinstance(data.get("runs"), list) else
            (len(data.get("results", [])) if isinstance(data.get("results"), list) else 3))
        # jvm_summary uses "runs" as list of per-JVM results
        data["meta"] = meta(3)
        with open(path, "w") as f:
            json.dump(data, f, indent=1)
            f.write("\n")
        print(f"[add_meta] dict root: meta added -> {path}")
    elif isinstance(data, list):
        # keep list format; write sibling meta
        sib = os.path.splitext(path)[0] + "_meta.json"
        with open(sib, "w") as f:
            json.dump(meta(3), f, indent=1)
            f.write("\n")
        print(f"[add_meta] list root ({len(data)} entries): sibling meta -> {sib}")
    else:
        print(f"[add_meta] SKIP (scalar root): {path}")
