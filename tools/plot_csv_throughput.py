#!/usr/bin/env python3
"""
Plot per-flow throughput vs time from CSV (matplotlib).

CSV columns: flow_id, timestamp_count, pps, bps, timestamps_sec
(timestamps_sec: semicolon-separated receive timestamps in seconds).

Writes: <repo>/figures/<csv_basename>.png (same stem as the input CSV).

Rows may appear in any order; curves and legend are ordered by ascending flow_id.
By default, vertical lines mark each flow's first/last timestamp (same color as the
curve; dashed = join, dotted = leave). Disable with --no-join-leave-lines.

The X axis is relative time (s): each timestamp minus the earliest first-packet time
in the CSV (minimum over flows of each flow's first timestamp in timestamps_sec).

Requires: matplotlib
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


def _ensure_csv_field_limit() -> None:
    """Allow very large CSV fields (e.g. millions of timestamps in one cell).

    Default csv limit is 128 KiB; timestamps_sec alone can be tens of MiB.
    """
    try:
        csv.field_size_limit(sys.maxsize)
    except OverflowError:
        # 32-bit C long (some Windows builds)
        csv.field_size_limit(min(sys.maxsize, 2**31 - 1))


def parse_timestamps(field: str) -> list[float]:
    s = field.strip().strip('"')
    if not s:
        return []
    return [float(x) for x in s.split(";") if x.strip()]


def binned_throughput_bps(
    times: list[float], row_bps: float, row_pps: float, n_bins: int
) -> tuple[list[float], list[float]]:
    """Bin centers (s) and throughput (bit/s) per bin."""
    if not times:
        return [], []
    times = sorted(times)
    t0, t1 = times[0], times[-1]
    span = t1 - t0
    bpp = (row_bps / row_pps) if row_pps > 0 else 0.0

    if span <= 0:
        width = 1e-6
        rate = len(times) * bpp / width
        return [t0], [rate]

    n_bins = max(1, min(n_bins, len(times)))
    width = span / n_bins
    counts = [0] * n_bins
    for t in times:
        if t >= t1:
            idx = n_bins - 1
        else:
            idx = int((t - t0) / width)
            idx = min(max(idx, 0), n_bins - 1)
        counts[idx] += 1

    centers = [t0 + (i + 0.5) * width for i in range(n_bins)]
    bps_series = [(c / width) * bpp for c in counts]
    return centers, bps_series


def load_series(
    csv_path: Path, n_bins: int
) -> list[tuple[int, list[float], list[float], float, float]]:
    """Per flow: flow_id, bin centers (s), throughput (Gbps series), join time, leave time."""
    _ensure_csv_field_limit()
    series: list[tuple[int, list[float], list[float], float, float]] = []
    with csv_path.open(newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        if reader.fieldnames is None:
            raise ValueError("empty CSV")
        for row in reader:
            try:
                fid = int(row["flow_id"])
                pps = float(row["pps"])
                bps = float(row["bps"])
            except (KeyError, ValueError) as e:
                raise ValueError(f"bad row {row!r}: {e}") from e
            ts = parse_timestamps(row.get("timestamps_sec", ""))
            if not ts:
                continue
            t_join = min(ts)
            t_leave = max(ts)
            centers, tbps = binned_throughput_bps(ts, bps, pps, n_bins)
            if not centers:
                continue
            gbps = [v / 1e9 for v in tbps]
            series.append((fid, centers, gbps, t_join, t_leave))
    return series


def plot_throughput(
    series: list[tuple[int, list[float], list[float], float, float]],
    out_path: Path,
    *,
    show_join_leave_lines: bool = True,
) -> None:
    # Legend order: ascending flow_id (CSV row order may be arbitrary).
    series = sorted(series, key=lambda s: s[0])
    # Earliest first-packet time in the file (same as global min timestamp).
    t_ref = min(s[3] for s in series)

    plt.figure(figsize=(10, 5))
    ax = plt.gca()

    for idx, (fid, centers, gbps, t_join, t_leave) in enumerate(series):
        color = f"C{idx % 10}"
        x_rel = [c - t_ref for c in centers]
        (line,) = ax.plot(
            x_rel, gbps, color=color, label=f"Flow ID {fid}", linewidth=1.2
        )
        color = line.get_color()
        if show_join_leave_lines:
            ax.axvline(
                t_join - t_ref,
                color=color,
                linestyle="--",
                linewidth=1.0,
                alpha=0.85,
                zorder=1,
            )
            ax.axvline(
                t_leave - t_ref,
                color=color,
                linestyle=":",
                linewidth=1.0,
                alpha=0.85,
                zorder=1,
            )

    plt.xlabel("Relative time (s)")
    plt.ylabel("Throughput (Gbps)")
    plt.title("Per-flow throughput over time")
    # Series are iterated in ascending flow_id order so the legend matches.
    ax.legend(loc="best")
    plt.grid(True, alpha=0.3)
    plt.tight_layout()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    plt.savefig(out_path, dpi=150)
    plt.close()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Plot per-flow throughput vs time from CSV (matplotlib)."
    )
    parser.add_argument(
        "csv_path",
        nargs="?",
        default="output/csv_output.csv",
        help="Input CSV path (default: output/csv_output.csv)",
    )
    parser.add_argument(
        "--bins",
        type=int,
        default=100,
        help="Number of time bins per flow (default: 100)",
    )
    parser.add_argument(
        "--figures-dir",
        type=Path,
        default=None,
        help="Output directory (default: <repo>/figures)",
    )
    parser.add_argument(
        "--no-join-leave-lines",
        dest="join_leave_lines",
        action="store_false",
        default=True,
        help="Do not draw join/leave vertical lines (default: draw them)",
    )
    args = parser.parse_args()

    csv_path = Path(args.csv_path).resolve()
    if not csv_path.is_file():
        print(f"error: CSV not found: {csv_path}", file=sys.stderr)
        return 1

    repo_root = Path(__file__).resolve().parent.parent
    figures_dir = (
        args.figures_dir.resolve()
        if args.figures_dir is not None
        else (repo_root / "figures")
    )

    try:
        series = load_series(csv_path, args.bins)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    if not series:
        print("error: no plottable rows", file=sys.stderr)
        return 1

    out_path = figures_dir / f"{csv_path.stem}.png"
    plot_throughput(
        series, out_path, show_join_leave_lines=args.join_leave_lines
    )
    print(f"Wrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
