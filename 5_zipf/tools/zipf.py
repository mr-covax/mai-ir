import argparse
import json
import math
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


def plot(source, output):
    rows = []
    seen = set()
    with source.open(encoding="utf-8") as stream:
        for line in stream:
            term, frequency, documents = line.rstrip("\n").split("\t")
            frequency, documents = int(frequency), int(documents)
            if term in seen or not term or frequency < documents or documents < 1:
                raise ValueError("Invalid term frequencies")
            seen.add(term)
            rows.append((term, frequency, documents))
    del seen
    rows.sort(key=lambda row: (-row[1], row[0]))
    if not rows:
        raise ValueError("Empty vocabulary")
    size = len(rows)
    tokens = sum(row[1] for row in rows)
    constant = tokens / sum(1 / rank for rank in range(1, size + 1))
    sx = sy = sxx = sxy = 0.0
    for rank, (_, count, _) in enumerate(rows, 1):
        x, y = math.log(rank), math.log(count)
        sx += x
        sy += y
        sxx += x * x
        sxy += x * y
    variance = sxx - sx * sx / size
    slope = (sxy - sx * sy / size) / variance if variance else 0
    points = min(size, 5000)
    ranks = sorted(
        {
            1,
            size,
            *(
                max(1, round(math.exp(math.log(size) * i / max(1, points - 1))))
                for i in range(points)
            ),
        }
    )
    counts = [rows[rank - 1][1] for rank in ranks]
    plt.figure(figsize=(8, 4.8))
    plt.loglog(ranks, counts, label="Корпус, выборка рангов для графика", linewidth=1.3)
    plt.loglog(
        ranks,
        [constant / rank for rank in ranks],
        label=f"Ципф: C/r, C={constant:.1f}",
        linestyle="--",
    )
    plt.xlabel("Ранг термина")
    plt.ylabel("Число вхождений")
    plt.grid(alpha=0.25, which="both")
    plt.legend()
    plt.tight_layout()
    plt.savefig(output.with_suffix(".png"), dpi=180)
    result = dict(
        terms=size,
        tokens=tokens,
        zipf_constant=constant,
        log_log_slope=slope,
        hapax=sum(count == 1 for _, count, _ in rows),
        top=rows[:20],
        plot_points=len(ranks),
        regression_uses_all_terms=True,
    )
    output.with_suffix(".json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("frequencies", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    plot(args.frequencies, args.output)
