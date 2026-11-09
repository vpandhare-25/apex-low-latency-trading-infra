"""Draws the README charts from the measured results in results/*.csv.

Usage (from the repo root):
    python3 -m venv .venv && .venv/bin/pip install -r scripts/requirements.txt
    .venv/bin/python scripts/plot_results.py

Writes results/charts/<name>-light.png and <name>-dark.png. The README shows the
version that matches the reader's GitHub theme.

Chart style: thin bars with a 2 px surface-coloured gap between neighbours, a light
recessive grid, no top/right frame, values written on the bars (so no number depends
on reading colour), and a colour-blind-safe palette that was checked with a validator
in both themes.
"""

import csv
import os
import statistics

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(ROOT, "results")
OUT = os.path.join(RESULTS, "charts")

# Colour tokens per theme. Series colours are categorical slots 1-3 (blue, orange, aqua).
THEMES = {
    "light": {
        "surface": "#fcfcfb",
        "text": "#0b0b0b",
        "muted": "#52514e",
        "grid": "#e4e3df",
        "series": ["#2a78d6", "#eb6834", "#1baf7a"],
    },
    "dark": {
        "surface": "#1a1a19",
        "text": "#f0efec",
        "muted": "#c3c2b7",
        "grid": "#3a3936",
        "series": ["#3987e5", "#d95926", "#199e70"],
    },
}


def read_csv(name):
    with open(os.path.join(RESULTS, name), newline="") as f:
        return list(csv.DictReader(f))


def style_axes(ax, t, grid_axis="y"):
    ax.set_facecolor(t["surface"])
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(t["grid"])
    ax.tick_params(colors=t["muted"], labelsize=9, length=0)
    ax.grid(axis=grid_axis, color=t["grid"], linewidth=0.8)
    ax.set_axisbelow(True)


def new_figure(t, width, height, ncols=1, nrows=1):
    fig, axes = plt.subplots(nrows, ncols, figsize=(width, height), squeeze=False)
    fig.patch.set_facecolor(t["surface"])
    for ax in axes.flat:
        style_axes(ax, t)
    return fig, axes


def title(fig, t, text, subtitle):
    fig.text(0.01, 0.97, text, ha="left", va="top", fontsize=13, fontweight="bold", color=t["text"])
    fig.text(0.01, 0.905, subtitle, ha="left", va="top", fontsize=9, color=t["muted"])


def save(fig, name, theme):
    os.makedirs(OUT, exist_ok=True)
    fig.savefig(os.path.join(OUT, f"{name}-{theme}.png"), dpi=200, facecolor=fig.get_facecolor())
    plt.close(fig)


# ---- 1. Order book latency percentiles ------------------------------------------------

def chart_book_latency(theme):
    t = THEMES[theme]
    rows = {r["book"]: r for r in read_csv("book_latency_aapl.csv")}
    books = ["OrderBook (std::map)", "FastOrderBook", "Hybrid N=16"]
    percentiles = [("p50", "p50"), ("p99", "p99"), ("p99_9", "p99.9")]

    fig, axes = new_figure(t, 8, 4.2)
    ax = axes[0][0]
    width = 0.26
    for i, book in enumerate(books):
        xs = [k + (i - 1) * width for k in range(len(percentiles))]
        values = [int(rows[book][col]) for col, _ in percentiles]
        ax.bar(xs, values, width, color=t["series"][i], edgecolor=t["surface"], linewidth=2, label=book)
        for x, v in zip(xs, values):
            ax.text(x, v + 12, f"{v}", ha="center", va="bottom", fontsize=8.5, color=t["text"])
    ax.set_xticks(range(len(percentiles)))
    ax.set_xticklabels([label for _, label in percentiles], fontsize=10, color=t["text"])
    ax.set_ylabel("ns per book update", color=t["muted"], fontsize=9)
    ax.set_ylim(0, 1080)
    legend = ax.legend(frameon=False, fontsize=9, loc="upper left")
    for text in legend.get_texts():
        text.set_color(t["text"])
    title(fig, t, "Order book update latency (lower is better)",
          "Real AAPL day, 1.51 M updates, every update timed; median of 3 rounds. Apple M1, single thread.")
    fig.subplots_adjust(top=0.80, left=0.09, right=0.98, bottom=0.10)
    save(fig, "book_latency", theme)


# ---- 2. Order book throughput -----------------------------------------------------------

def chart_book_throughput(theme):
    t = THEMES[theme]
    rows = {r["book"]: r for r in read_csv("book_throughput_aapl.csv")}
    books = ["OrderBook (std::map)", "FlatOrderBook", "FastOrderBook", "Hybrid N=16"]
    notes = ["baseline", "sorted-vector levels", "+ open-addressing hash map", "+ top 16 levels in vector, rest in tree"]

    fig, axes = new_figure(t, 8, 3.6)
    ax = axes[0][0]
    style_axes(ax, t, grid_axis="x")
    ys = list(range(len(books)))[::-1]
    values = [float(rows[b]["replay_ms"]) for b in books]
    ax.barh(ys, values, 0.55, color=t["series"][0], edgecolor=t["surface"], linewidth=2)
    base = values[0]
    for y, v, b in zip(ys, values, books):
        rate = float(rows[b]["events_per_sec_millions"])
        ax.text(v + 3, y, f"{v:.0f} ms  ({rate:.1f} M/s, {base / v:.2f}x)", va="center", fontsize=8.5, color=t["text"])
    ax.set_yticks(ys)
    ax.set_yticklabels([f"{b}\n{n}" for b, n in zip(books, notes)], fontsize=8.5, color=t["text"])
    ax.set_xlim(0, 300)
    ax.set_xlabel("ms to replay the whole AAPL day (lower is better)", color=t["muted"], fontsize=9)
    title(fig, t, "Order book throughput", "Google Benchmark, 1.51 M real AAPL updates in memory, median of 10.")
    fig.subplots_adjust(top=0.80, left=0.33, right=0.98, bottom=0.14)
    save(fig, "book_throughput", theme)


# ---- 3. Routers: cost and fill rate right after quote changes ---------------------------

def chart_router(theme):
    t = THEMES[theme]
    rows = [r for r in read_csv("router.csv") if r["schedule"] == "after quote change"]
    strategies = ["single-venue", "split", "split+fees", "split+fees+reroute"]

    fig, axes = new_figure(t, 9, 5.4, ncols=2, nrows=2)
    for row_i, symbol in enumerate(["AAPL", "MSFT"]):
        data = {r["strategy"]: r for r in rows if r["symbol"] == symbol}
        ys = list(range(len(strategies)))[::-1]

        # Left: cost in bps (bars start at zero).
        ax = axes[row_i][0]
        style_axes(ax, t, grid_axis="x")
        costs = [float(data[s]["cost_bps"]) for s in strategies]
        ax.barh(ys, costs, 0.55, color=t["series"][0], edgecolor=t["surface"], linewidth=2)
        for y, c in zip(ys, costs):
            ax.text(c + 0.03, y, f"{c:.3f} bps", va="center", fontsize=8.5, color=t["text"])
        ax.set_xlim(0, 2.6)
        ax.set_yticks(ys)
        ax.set_yticklabels(strategies, fontsize=9, color=t["text"])
        ax.set_title(f"{symbol}: cost vs arrival mid (lower is better)", loc="left", fontsize=9.5, color=t["text"])

        # Right: fill rate as dots (a dot plot need not start at zero, so small gaps show).
        ax = axes[row_i][1]
        style_axes(ax, t, grid_axis="x")
        fills = [float(data[s]["fill_rate_pct"]) for s in strategies]
        ax.scatter(fills, ys, s=70, color=t["series"][1], edgecolor=t["surface"], linewidth=2, zorder=3)
        for y, f in zip(ys, fills):
            ax.text(f - 0.25, y + 0.28, f"{f:.2f}%", ha="center", fontsize=8.5, color=t["text"])
        ax.set_xlim(94, 100.6)
        ax.set_ylim(-0.6, len(strategies) - 0.3)
        ax.set_yticks(ys)
        ax.set_yticklabels([])
        ax.set_title(f"{symbol}: fill rate (higher is better)", loc="left", fontsize=9.5, color=t["text"])

    title(fig, t, "Routers, right after a quote change",
          "~700 paired 1,000-share orders per stock on 2019-12-30. Splitting saves cost; re-routing fixes fills.")
    fig.subplots_adjust(top=0.84, left=0.17, right=0.98, bottom=0.06, hspace=0.45, wspace=0.08)
    save(fig, "router", theme)


# ---- 4. Pipeline speedup ----------------------------------------------------------------

def chart_pipeline(theme):
    t = THEMES[theme]
    rows = read_csv("pipeline.csv")
    base = statistics.median(float(rows[0][k]) for k in ("round1_s", "round2_s", "round3_s"))

    fig, axes = new_figure(t, 8.5, 4.4)
    ax = axes[0][0]
    style_axes(ax, t, grid_axis="x")
    ys = list(range(len(rows)))[::-1]
    for y, r in zip(ys, rows):
        times = [float(r[k]) for k in ("round1_s", "round2_s", "round3_s")]
        median = statistics.median(times)
        speedups = [base / x for x in times]
        ax.barh(y, base / median, 0.55, color=t["series"][0], edgecolor=t["surface"], linewidth=2)
        ax.plot([min(speedups), max(speedups)], [y, y], color=t["text"], linewidth=1.2, zorder=3)
        ax.text(max(speedups) + 0.05, y, f"{base / median:.2f}x  ({median:.1f} s)", va="center",
                fontsize=8.5, color=t["text"])
    ax.axvline(1.0, color=t["muted"], linewidth=1, linestyle=(0, (3, 3)))
    ax.set_yticks(ys)
    ax.set_yticklabels([f"{r['threads']} thr  {r['layout']}" for r in rows], fontsize=8.5, color=t["text"])
    ax.set_xlim(0, 2.6)
    ax.set_xlabel("speedup vs 1 thread (bar = median of 3 rounds, line = range)", color=t["muted"], fontsize=9)
    title(fig, t, "Pipeline: books for all 8,892 stocks from the full 3.5 GB day",
          "Sharding the bottleneck stage (books) is what pays off. Apple M1: 4 fast + 4 efficiency cores.")
    fig.subplots_adjust(top=0.82, left=0.36, right=0.98, bottom=0.12)
    save(fig, "pipeline", theme)


if __name__ == "__main__":
    for theme in THEMES:
        chart_book_latency(theme)
        chart_book_throughput(theme)
        chart_router(theme)
        chart_pipeline(theme)
    print("wrote", sorted(os.listdir(OUT)))
