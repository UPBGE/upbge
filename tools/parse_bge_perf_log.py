#!/usr/bin/env python3
"""
Parseador de logs de performance do UPBGE (Milestone 1).

Uso:
    upbge.exe > bge_perf.log 2>&1
    python tools/parse_bge_perf_log.py bge_perf.log

Ou pipe:
    upbge.exe 2>&1 | python tools/parse_bge_perf_log.py -

Saída:
    Estatísticas agregadas por categoria (GetFrameTimes, NextFrame,
    UpdateDepsgraph, ProceedDeltaTime) com média, mediana, p99, min, max e total.
"""

import sys
import re
import statistics
from pathlib import Path
from collections import defaultdict


def parse_log(lines):
    """Parse [BGE_PERF] log lines into structured records."""
    records = []
    # Pattern: [BGE_PERF] <category> key=value ...
    prefix_re = re.compile(r"^\[BGE_PERF\]\s+(.+)$")
    kv_re = re.compile(r"(\w+)=(\S+)")

    for line in lines:
        line = line.strip()
        m = prefix_re.match(line)
        if not m:
            continue
        payload = m.group(1)
        kvs = kv_re.findall(payload)
        entry = {}
        for key, value in kvs:
            try:
                # Try numeric conversion; keep strings if it fails.
                entry[key] = float(value) if "." in value else int(value)
            except ValueError:
                entry[key] = value
        # Derive category from the first token before any key=value.
        category = payload.split()[0] if kvs else payload.split()[0]
        entry["category"] = category
        records.append(entry)

    return records


def aggregate(records):
    """Aggregate records by category and compute statistics."""
    groups = defaultdict(list)
    for r in records:
        cat = r.get("category", "UNKNOWN")
        if "time_ms" in r:
            groups[cat].append(r["time_ms"])

    stats = {}
    for cat, values in groups.items():
        if not values:
            continue
        values_sorted = sorted(values)
        n = len(values)
        mean = statistics.mean(values)
        median = statistics.median(values)
        p99 = values_sorted[int(n * 0.99)] if n > 1 else values[0]
        stats[cat] = {
            "count": n,
            "mean": mean,
            "median": median,
            "p99": p99,
            "min": min(values),
            "max": max(values),
            "total": sum(values),
        }
    return stats


def print_stats(stats, out=sys.stdout):
    """Print formatted statistics."""
    out.write("UPBGE Performance Baseline (Milestone 1)\n")
    out.write("=" * 70 + "\n")
    header = f"{'Category':<25} {'Count':>8} {'Mean':>10} {'Median':>10} {'P99':>10} {'Min':>10} {'Max':>10} {'Total':>12}\n"
    out.write(header)
    out.write("-" * 70 + "\n")
    for cat in sorted(stats.keys()):
        s = stats[cat]
        out.write(
            f"{cat:<25} {s['count']:>8} {s['mean']:>10.3f} {s['median']:>10.3f} "
            f"{s['p99']:>10.3f} {s['min']:>10.3f} {s['max']:>10.3f} {s['total']:>12.3f}\n"
        )
    out.write("\n")
    out.write("Dicas:\n")
    out.write("- 'NextFrame total_ms' é o tempo bruto de um frame completo.\n")
    out.write("- 'ProceedDeltaTime' é o tempo do Bullet + sync.\n")
    out.write("- 'UpdateDepsgraph' é o overhead do depsgraph por cena.\n")
    out.write("- 'GetFrameTimes' mostra dt, timestep, frames e framestep.\n")


def main():
    if len(sys.argv) < 2:
        print(f"Uso: {sys.argv[0]} <arquivo.log> (ou '-' para stdin)")
        sys.exit(1)

    path = sys.argv[1]
    if path == "-":
        lines = sys.stdin.readlines()
    else:
        lines = Path(path).read_text(encoding="utf-8", errors="replace").splitlines()

    records = parse_log(lines)
    stats = aggregate(records)
    print_stats(stats)


if __name__ == "__main__":
    main()
