#!/usr/bin/env python3
"""assemble_trace_summary.py — build baseline_trace_summary.json from
trace_processor CSV outputs (q1/q3/q5) + the workload's own JSON line.
Run on host after the 30s pf_high8 trace capture. Idempotent."""
import csv, io, json, os, sys

BASE = os.path.dirname(os.path.abspath(__file__))

def read_csv(path):
    """trace_processor query prints result sets as CSV blocks separated by
    blank lines; return list of (header, rows)."""
    if not os.path.exists(path):
        return []
    txt = open(path).read()
    sets = []
    for block in txt.split("\n\n"):
        lines = [l for l in block.strip().splitlines() if l.strip()]
        if not lines or not lines[0].startswith('"'):
            continue
        rd = csv.DictReader(io.StringIO("\n".join(lines)))
        sets.append((rd.fieldnames, list(rd)))
    return sets

q1 = read_csv(os.path.join(BASE, "q1_out.csv"))
q3 = read_csv(os.path.join(BASE, "q3_out.csv"))
q5 = read_csv(os.path.join(BASE, "q5_out.csv"))

counts = {r["name"]: int(r["n"]) for h, rows in q1 for r in rows if h and "name" in h}
win = next((r for h, rows in q1 for r in rows if h and "covered_window_s" in h), None)

wait_v2 = next((r for h, rows in q3 for r in rows
                if h and r.get("section") == "WAIT_OVERALL_V2"), None)
pf_by_tid = [r for h, rows in q3 for r in rows if h and r.get("section") == "PF_BY_THREAD"]
dump = [r for h, rows in q3 for r in rows if h and r.get("section") == "MMAP_LOCK_DUMP"]

hold = [r for h, rows in q5 for r in rows if h and "section" in h]
hold_overall = [r for r in hold if r["utid"] == "*"]
hold_by_utid = [r for r in hold if r["utid"] != "*"]

wl = json.load(open(os.path.join(BASE, "pf_trace_workload.json")))

def num(x):
    return float(x) if x is not None else None

covered = num(win["covered_window_s"])
pf_total = counts.get("page_fault_user", 0)
ml_total = sum(counts.get(n, 0) for n in
               ("mmap_lock_start_locking", "mmap_lock_acquire_returned", "mmap_lock_released"))

summary = {
    "bench_set": "perfetto 30s high-contention PF trace baseline (mmbench pf high 8 threads)",
    "meta": {
        "kernel": "6.18.32-g68974e235117",
        "bzimage_sha256": "038ed5b38e76947b768006cb3171c231ea4acd5a3cf3515cd2560273776c2be9",
        "runs": 1,
        "date": "2026-09-13",
        "qemu": "8vCPU/4G/KVM",
        "perfetto": "v58.2 tracebox (guest) / trace_processor_shell v58.2 (host)",
        "workload_cmd": "/mnt/mmbench pf high 8 30 42",
        "trace_cmd": "./tracebox --txt -c /mnt/trace_cfg.txt -o /mnt/baseline/pf_high8.pftrace",
        "trace_cfg": "events: sched/sched_switch, sched/sched_wakeup, mmap_lock/{start_locking,acquire_returned,released}, exceptions/page_fault_user; 32MB x 8 cpu ring buffer; duration_ms 30000",
    },
    "workload": {
        **wl,
        "note": "ops_per_us during tracing is ~25% below the untraced 3-run median "
                "(0.0419) — observational overhead of ftrace event capture, expected.",
    },
    "trace_capture": {
        "requested_duration_s": 30,
        "covered_window_s": covered,
        "ts_min_ns": int(win["ts_min"]),
        "ts_max_ns": int(win["ts_max"]),
        "trace_file_bytes": os.path.getsize(os.path.join(BASE, "pf_high8.pftrace")),
        "note": "RING_BUFFER policy: when the ring fills, oldest events are overwritten, "
                "so the analyzed window is the tail (~%.2fs) of the 30s run in steady state; "
                "per-second rates below are steady-state rates." % covered,
    },
    "events_total": sum(counts.values()),
    "events_by_name": counts,
    "mmap_lock": {
        "total_events": ml_total,
        "start_locking": counts.get("mmap_lock_start_locking", 0),
        "acquire_returned": counts.get("mmap_lock_acquire_returned", 0),
        "released": counts.get("mmap_lock_released", 0),
        "key_finding": "On 6.18 (CONFIG_PER_VMA_LOCK), mmbench pf page faults are served by "
                       "per-VMA read locks and the speculative path, NOT by mmap_lock: only 50 "
                       "mmap_lock events (%d releases = %d arena-reset cycles x3) occur in the "
                       "covered window vs %d page faults. The few acquisitions are the "
                       "benchmark's periodic arena reset (munmap 718MB + fresh mmap) and short "
                       "read locks."
                       % (counts.get("mmap_lock_released", 0),
                          counts.get("mmap_lock_released", 0) // 3, pf_total),
        "reset_anatomy": "per reset cycle: munmap takes mmap_lock write (~7-19us), "
                         "mmap_write_downgrade() downgrades to read (acquire_returned without "
                         "start_locking), read lock is held through the ~95-104ms page-table "
                         "teardown of the 718MB arena, then released; the fresh mmap takes write "
                         "~19us; plus one ~3-9us ordinary read lock.",
        "wait_ns_overall": {  # start_locking -> first acquire_returned (same tid, ts >= start)
            "method": "first acquire_returned at ts >= start_locking per tid (approximate)",
            "pairs": 15, "mean_ns": num(wait_v2["mean_ns"]), "min_ns": num(wait_v2["min_ns"]),
            "max_ns": num(wait_v2["max_ns"]),
            "interpretation": "mmap_lock effectively uncontended in this workload on 8 vCPU",
        },
        "hold_sections_ns_overall": hold_overall,
        "hold_sections_ns_by_utid": hold_by_utid,
        "pairing_method": "downgrade-aware: write sections pair write-acquire -> "
                          "{downgrade | write-release}; read sections pair read-acquire "
                          "(incl. downgrades) -> read-release, FIFO per tid. Raw event dump "
                          "below enables manual audit (50 events).",
        "raw_event_dump": dump,
    },
    "page_fault": {
        "total_in_window": pf_total,
        "rate_in_window_per_s": round(pf_total / covered, 1) if covered else None,
        "full_run_estimate_per_s": round(4 * wl["ops"] / wl["elapsed_s"], 1),
        "by_tid": pf_by_tid,
    },
    "sched": {
        "sched_switch": counts.get("sched_switch", 0),
        "sched_wakeup": counts.get("sched_wakeup", 0),
        "interpretation": "near-zero scheduling churn: 8 worker threads on 8 vCPUs stay "
                          "runnable inside the fault path; no lock-sleep traffic",
    },
    "caveats": [
        "ring buffer holds only the last ~%.2fs of the 30s trace; counts are steady-state "
        "window counts, not 30s totals (normalize by rate)" % covered,
        "n=15 lock sections is small; latency percentiles here characterize the arena-reset "
        "path, not fault-path mmap_lock latency (faults bypass mmap_lock on 6.18)",
        "THP is absent in this kernel build (no TRANSPARENT_HUGEPAGE); M3+ arena design must "
        "assume 4KB pages",
    ],
}

out = os.path.join(BASE, "baseline_trace_summary.json")
with open(out, "w") as f:
    json.dump(summary, f, indent=1)
    f.write("\n")
print("wrote", out)
