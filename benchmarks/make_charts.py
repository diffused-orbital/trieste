#!/usr/bin/env python3
"""Render the M5 benchmark CSVs as SVG charts.

Self-contained on purpose: it writes SVG text directly rather than pulling in
matplotlib, so regenerating the charts needs nothing but a Python interpreter
and keeps the repository free of a plotting toolchain.

    python benchmarks/make_charts.py

Reads  benchmarks/results/*.csv
Writes benchmarks/results/*.svg

Each chart paints its own light background so it stays readable in both GitHub
light and dark themes, where a transparent SVG would otherwise render dark text
on a dark page.
"""

import csv
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
RESULTS = os.path.join(HERE, "results")

BG = "#ffffff"
FG = "#1f2328"
MUTED = "#6e7781"
GRID = "#d8dee4"
SERIES = ["#0969da", "#cf222e", "#1a7f37", "#8250df"]

W, H = 760, 420
PAD_L, PAD_R, PAD_T, PAD_B = 78, 26, 52, 62


def esc(s):
    return str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def header(title, subtitle):
    return [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
        f'viewBox="0 0 {W} {H}" font-family="-apple-system,Segoe UI,Helvetica,Arial,sans-serif">',
        f'<rect width="{W}" height="{H}" fill="{BG}"/>',
        f'<text x="{PAD_L}" y="26" font-size="16" font-weight="600" fill="{FG}">{esc(title)}</text>',
        f'<text x="{PAD_L}" y="44" font-size="11.5" fill="{MUTED}">{esc(subtitle)}</text>',
    ]


def log_ticks(lo, hi):
    """Decade ticks spanning [lo, hi]."""
    ticks = []
    e = math.floor(math.log10(lo))
    while 10 ** e <= hi * 1.001:
        if 10 ** e >= lo * 0.999:
            ticks.append(10 ** e)
        e += 1
    return ticks or [lo, hi]


def fmt_us(v):
    """Axis-tick formatting: decades come out as round numbers."""
    if v >= 1000:
        return f"{v / 1000:g} ms"
    if v >= 1:
        return f"{v:g} us"
    return f"{v:.2g} us"


def fmt_val(v):
    """Data-label formatting: three significant figures, no long tails."""
    if v >= 1000:
        return f"{v / 1000:.2f} ms"
    if v >= 100:
        return f"{v:.0f} us"
    return f"{v:.1f} us"


# ---------------------------------------------------------------------------
# 1. Naive full-dictionary Levenshtein vs the trie-pruned walk
# ---------------------------------------------------------------------------
def chart_naive_vs_pruned():
    rows = list(csv.DictReader(open(os.path.join(RESULTS, "naive_vs_pruned.csv"))))
    data = {(r["edit_budget"], r["algorithm"]): float(r["p50_us"]) for r in rows}
    budgets = ["1", "2"]

    lo, hi = 10.0, 20000.0
    x0, x1 = PAD_L, W - PAD_R
    y0, y1 = PAD_T, H - PAD_B

    def ypos(v):
        t = (math.log10(v) - math.log10(lo)) / (math.log10(hi) - math.log10(lo))
        return y1 - t * (y1 - y0)

    out = header(
        "Fuzzy search: naive full-dictionary vs trie-pruned",
        "p50 latency per query, 108,008-term English corpus, log scale. Same queries, "
        "identical results (0 mismatches).",
    )
    for t in log_ticks(lo, hi):
        y = ypos(t)
        out.append(f'<line x1="{x0}" y1="{y:.1f}" x2="{x1}" y2="{y:.1f}" stroke="{GRID}"/>')
        out.append(
            f'<text x="{x0 - 8}" y="{y + 4:.1f}" font-size="11" fill="{MUTED}" '
            f'text-anchor="end">{fmt_us(t)}</text>'
        )

    group_w = (x1 - x0) / len(budgets)
    bar_w = 74
    for gi, b in enumerate(budgets):
        cx = x0 + group_w * (gi + 0.5)
        for si, alg in enumerate(["Naive", "TriePruned"]):
            v = data[(b, alg)]
            bx = cx - bar_w - 10 + si * (bar_w + 20)
            by = ypos(v)
            out.append(
                f'<rect x="{bx:.1f}" y="{by:.1f}" width="{bar_w}" height="{y1 - by:.1f}" '
                f'fill="{SERIES[si]}" rx="3"/>'
            )
            out.append(
                f'<text x="{bx + bar_w / 2:.1f}" y="{by - 7:.1f}" font-size="12" '
                f'font-weight="600" fill="{FG}" text-anchor="middle">{fmt_val(v)}</text>'
            )
        speed = data[(b, "Naive")] / data[(b, "TriePruned")]
        out.append(
            f'<text x="{cx:.1f}" y="{y1 + 22:.1f}" font-size="12.5" fill="{FG}" '
            f'text-anchor="middle">E = {b}</text>'
        )
        out.append(
            f'<text x="{cx:.1f}" y="{y1 + 41:.1f}" font-size="13" font-weight="700" '
            f'fill="{SERIES[2]}" text-anchor="middle">{speed:.0f}x faster</text>'
        )

    out.append(f'<line x1="{x0}" y1="{y1}" x2="{x1}" y2="{y1}" stroke="{FG}" stroke-width="1.2"/>')
    for si, lbl in enumerate(["Naive O(N*M) scan", "Trie + pruned DP walk"]):
        lx = x0 + 12 + si * 210
        out.append(f'<rect x="{lx}" y="{PAD_T - 18}" width="11" height="11" fill="{SERIES[si]}" rx="2"/>')
        out.append(
            f'<text x="{lx + 17}" y="{PAD_T - 8}" font-size="11.5" fill="{FG}">{esc(lbl)}</text>'
        )
    out.append("</svg>")
    return "\n".join(out)


# ---------------------------------------------------------------------------
# 2. Exact prefix latency vs prefix length
# ---------------------------------------------------------------------------
def chart_latency_by_length():
    rows = [
        r
        for r in csv.DictReader(open(os.path.join(RESULTS, "latency.csv")))
        if r["benchmark"] == "BM_PrefixTopK_ByLength"
    ]
    rows.sort(key=lambda r: int(r["variant"]))
    xs = [int(r["variant"]) for r in rows]
    series = [
        ("p50", [float(r["p50_us"]) for r in rows], SERIES[0]),
        ("p95", [float(r["p95_us"]) for r in rows], SERIES[3]),
        ("p99", [float(r["p99_us"]) for r in rows], SERIES[1]),
    ]

    lo, hi = 0.4, 6000.0
    x0, x1 = PAD_L, W - PAD_R
    y0, y1 = PAD_T, H - PAD_B

    def ypos(v):
        t = (math.log10(v) - math.log10(lo)) / (math.log10(hi) - math.log10(lo))
        return y1 - t * (y1 - y0)

    def xpos(i):
        return x0 + (x1 - x0) * i / (len(xs) - 1)

    out = header(
        "Exact prefix Top-K latency vs prefix length",
        "k=5, edit budget 0. Short prefixes sit above huge subtrees and M1 walks all of "
        "them; the 2ms/5ms budget is drawn for reference.",
    )
    for t in log_ticks(lo, hi):
        y = ypos(t)
        out.append(f'<line x1="{x0}" y1="{y:.1f}" x2="{x1}" y2="{y:.1f}" stroke="{GRID}"/>')
        out.append(
            f'<text x="{x0 - 8}" y="{y + 4:.1f}" font-size="11" fill="{MUTED}" '
            f'text-anchor="end">{fmt_us(t)}</text>'
        )
    for budget, lbl in ((2000.0, "p95 target 2ms"), (5000.0, "p99 target 5ms")):
        y = ypos(budget)
        out.append(
            f'<line x1="{x0}" y1="{y:.1f}" x2="{x1}" y2="{y:.1f}" stroke="{MUTED}" '
            f'stroke-width="1" stroke-dasharray="5 4"/>'
        )
        out.append(
            f'<text x="{x1 - 4}" y="{y - 5:.1f}" font-size="10.5" fill="{MUTED}" '
            f'text-anchor="end">{esc(lbl)}</text>'
        )

    for name, vals, colour in series:
        pts = " ".join(f"{xpos(i):.1f},{ypos(v):.1f}" for i, v in enumerate(vals))
        out.append(f'<polyline points="{pts}" fill="none" stroke="{colour}" stroke-width="2.4"/>')
        for i, v in enumerate(vals):
            out.append(f'<circle cx="{xpos(i):.1f}" cy="{ypos(v):.1f}" r="3.6" fill="{colour}"/>')

    for i, x in enumerate(xs):
        out.append(
            f'<text x="{xpos(i):.1f}" y="{y1 + 22:.1f}" font-size="12" fill="{FG}" '
            f'text-anchor="middle">{x}</text>'
        )
    out.append(
        f'<text x="{(x0 + x1) / 2:.1f}" y="{y1 + 44:.1f}" font-size="12" fill="{MUTED}" '
        f'text-anchor="middle">prefix length (characters)</text>'
    )
    out.append(f'<line x1="{x0}" y1="{y1}" x2="{x1}" y2="{y1}" stroke="{FG}" stroke-width="1.2"/>')
    for si, (name, _, colour) in enumerate(series):
        lx = x0 + 12 + si * 66
        out.append(f'<rect x="{lx}" y="{PAD_T - 18}" width="11" height="11" fill="{colour}" rx="2"/>')
        out.append(f'<text x="{lx + 17}" y="{PAD_T - 8}" font-size="11.5" fill="{FG}">{name}</text>')
    out.append("</svg>")
    return "\n".join(out)


# ---------------------------------------------------------------------------
# 3. QPS vs thread count
# ---------------------------------------------------------------------------
def chart_qps():
    rows = list(csv.DictReader(open(os.path.join(RESULTS, "throughput.csv"))))
    by_workload = {}
    for r in rows:
        by_workload.setdefault(r["workload"], []).append((int(r["threads"]), float(r["qps"])))
    for v in by_workload.values():
        v.sort()

    threads = [1, 2, 4, 8, 16]
    hi = 240000.0
    x0, x1 = PAD_L, W - PAD_R
    y0, y1 = PAD_T, H - PAD_B

    def ypos(v):
        return y1 - (v / hi) * (y1 - y0)

    def xpos(t):
        return x0 + (x1 - x0) * (math.log2(t) / math.log2(16))

    out = header(
        "Throughput vs thread count (aggregate, wall-clock)",
        "Shared-lock read path. Dashed line is perfect scaling from the 2-thread point; "
        "the 1-thread baseline is depressed by laptop power management.",
    )
    for i in range(0, 6):
        v = hi * i / 5
        y = ypos(v)
        out.append(f'<line x1="{x0}" y1="{y:.1f}" x2="{x1}" y2="{y:.1f}" stroke="{GRID}"/>')
        out.append(
            f'<text x="{x0 - 8}" y="{y + 4:.1f}" font-size="11" fill="{MUTED}" '
            f'text-anchor="end">{v / 1000:.0f}k</text>'
        )

    base = dict(by_workload["PrefixReads"])[2] / 2.0
    pts = " ".join(f"{xpos(t):.1f},{ypos(min(base * t, hi)):.1f}" for t in threads if t >= 2)
    out.append(
        f'<polyline points="{pts}" fill="none" stroke="{MUTED}" stroke-width="1.4" '
        f'stroke-dasharray="6 5"/>'
    )

    for si, (name, series) in enumerate(sorted(by_workload.items())):
        colour = SERIES[si % len(SERIES)]
        pts = " ".join(f"{xpos(t):.1f},{ypos(q):.1f}" for t, q in series)
        out.append(f'<polyline points="{pts}" fill="none" stroke="{colour}" stroke-width="2.4"/>')
        for t, q in series:
            out.append(f'<circle cx="{xpos(t):.1f}" cy="{ypos(q):.1f}" r="3.8" fill="{colour}"/>')
        lt, lq = series[-1]
        out.append(
            f'<text x="{xpos(lt) - 6:.1f}" y="{ypos(lq) - 10:.1f}" font-size="11.5" '
            f'font-weight="600" fill="{colour}" text-anchor="end">{lq / 1000:.0f}k</text>'
        )

    for t in threads:
        out.append(
            f'<text x="{xpos(t):.1f}" y="{y1 + 22:.1f}" font-size="12" fill="{FG}" '
            f'text-anchor="middle">{t}</text>'
        )
    out.append(
        f'<text x="{(x0 + x1) / 2:.1f}" y="{y1 + 44:.1f}" font-size="12" fill="{MUTED}" '
        f'text-anchor="middle">reader threads</text>'
    )
    out.append(f'<line x1="{x0}" y1="{y1}" x2="{x1}" y2="{y1}" stroke="{FG}" stroke-width="1.2"/>')
    for si, name in enumerate(sorted(by_workload)):
        lx = x0 + 12 + si * 160
        out.append(
            f'<rect x="{lx}" y="{PAD_T - 18}" width="11" height="11" '
            f'fill="{SERIES[si % len(SERIES)]}" rx="2"/>'
        )
        out.append(f'<text x="{lx + 17}" y="{PAD_T - 8}" font-size="11.5" fill="{FG}">{esc(name)}</text>')
    out.append("</svg>")
    return "\n".join(out)


# ---------------------------------------------------------------------------
# 4. M6 before/after: exact prefix p95 by prefix length
# ---------------------------------------------------------------------------
def chart_m6_before_after():
    rows = [r for r in csv.DictReader(open(os.path.join(RESULTS, "m6_before_after.csv")))
            if r["dimension"] == "prefix_length"]
    rows.sort(key=lambda r: int(r["value"]))
    xs = [int(r["value"]) for r in rows]
    before = [float(r["p95_before_us"]) for r in rows]
    after = [float(r["p95_after_us"]) for r in rows]

    lo, hi = 1.0, 20000.0
    x0, x1 = PAD_L, W - PAD_R
    y0, y1 = PAD_T, H - PAD_B

    def ypos(v):
        t = (math.log10(v) - math.log10(lo)) / (math.log10(hi) - math.log10(lo))
        return y1 - t * (y1 - y0)

    out = header(
        "M6: exact prefix p95, before and after subtree-max best-first",
        "Log scale. The 2-character case -- the only measurement that missed its "
        "budget -- improves 506x and drops far under the 2ms line.",
    )
    for t in log_ticks(lo, hi):
        y = ypos(t)
        out.append(f'<line x1="{x0}" y1="{y:.1f}" x2="{x1}" y2="{y:.1f}" stroke="{GRID}"/>')
        out.append(
            f'<text x="{x0 - 8}" y="{y + 4:.1f}" font-size="11" fill="{MUTED}" '
            f'text-anchor="end">{fmt_us(t)}</text>'
        )
    y = ypos(2000.0)
    out.append(
        f'<line x1="{x0}" y1="{y:.1f}" x2="{x1}" y2="{y:.1f}" stroke="{MUTED}" '
        f'stroke-width="1" stroke-dasharray="5 4"/>'
    )
    out.append(
        f'<text x="{x1 - 4}" y="{y - 5:.1f}" font-size="10.5" fill="{MUTED}" '
        f'text-anchor="end">p95 target 2ms</text>'
    )

    group_w = (x1 - x0) / len(xs)
    bar_w = min(38.0, group_w / 2.6)
    for i, x in enumerate(xs):
        cx = x0 + group_w * (i + 0.5)
        for si, (vals, colour) in enumerate(((before, SERIES[1]), (after, SERIES[2]))):
            v = vals[i]
            bx = cx - bar_w - 4 + si * (bar_w + 8)
            by = ypos(v)
            out.append(
                f'<rect x="{bx:.1f}" y="{by:.1f}" width="{bar_w:.1f}" '
                f'height="{y1 - by:.1f}" fill="{colour}" rx="2"/>'
            )
        out.append(
            f'<text x="{cx:.1f}" y="{y1 + 22:.1f}" font-size="12" fill="{FG}" '
            f'text-anchor="middle">{x}</text>'
        )
        out.append(
            f'<text x="{cx:.1f}" y="{y1 + 40:.1f}" font-size="11.5" font-weight="700" '
            f'fill="{SERIES[2]}" text-anchor="middle">{before[i] / after[i]:.0f}x</text>'
        )

    out.append(f'<line x1="{x0}" y1="{y1}" x2="{x1}" y2="{y1}" stroke="{FG}" stroke-width="1.2"/>')
    out.append(
        f'<text x="{(x0 + x1) / 2:.1f}" y="{H - 8}" font-size="12" fill="{MUTED}" '
        f'text-anchor="middle">prefix length (characters)</text>'
    )
    for si, lbl in enumerate(["before (M5: full subtree walk)", "after (M6: best-first)"]):
        lx = x0 + 12 + si * 230
        out.append(
            f'<rect x="{lx}" y="{PAD_T - 18}" width="11" height="11" '
            f'fill="{SERIES[1] if si == 0 else SERIES[2]}" rx="2"/>'
        )
        out.append(f'<text x="{lx + 17}" y="{PAD_T - 8}" font-size="11.5" fill="{FG}">{esc(lbl)}</text>')
    out.append("</svg>")
    return "\n".join(out)


def main():
    charts = {
        "naive_vs_pruned.svg": chart_naive_vs_pruned(),
        "latency_by_prefix_length.svg": chart_latency_by_length(),
        "qps_vs_threads.svg": chart_qps(),
        "m6_before_after.svg": chart_m6_before_after(),
    }
    for name, svg in charts.items():
        path = os.path.join(RESULTS, name)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(svg)
        print(f"wrote {path}")


if __name__ == "__main__":
    main()
