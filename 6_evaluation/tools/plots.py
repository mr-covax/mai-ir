import argparse
import csv
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import PercentFormatter


CUTOFFS = ("1", "3", "5")
METRICS = (("P", "Precision"), ("DCG", "DCG"), ("NDCG", "NDCG"), ("ERR", "ERR"))


def save(figure, path):
    figure.savefig(path, dpi=180, facecolor="white")
    plt.close(figure)
    print(path)


def plot_metrics(source, output):
    data = json.loads(source.read_text(encoding="utf-8"))
    systems = data["systems"]
    if not systems:
        raise ValueError("В файле нет оценённых систем")
    names = list(systems)
    query_ids = sorted(systems[names[0]]["queries"])
    if not query_ids or any(
        set(system["queries"]) != set(query_ids) for system in systems.values()
    ):
        raise ValueError("Для всех систем требуется одинаковый непустой набор запросов")

    averages = [
        [
            [system["macro"][cutoff][metric] for cutoff in CUTOFFS]
            for system in systems.values()
        ]
        for metric, _ in METRICS
    ]
    per_query = [
        [system["queries"][query]["5"]["NDCG"] for query in query_ids]
        for system in systems.values()
    ]
    output.mkdir(parents=True, exist_ok=True)

    columns = min(4, len(names))
    figure, axes = plt.subplots(2, 2, figsize=(10, 7.4), layout="constrained")
    figure.suptitle("Средние метрики качества поиска")
    width = 0.8 / len(names)
    for axis, (metric, label), rows in zip(axes.flat, METRICS, averages):
        for i, (name, values) in enumerate(zip(names, rows)):
            shift = (i - (len(names) - 1) / 2) * width
            axis.bar(
                [x + shift for x in range(len(CUTOFFS))], values, width, label=name
            )
        axis.set(
            title=label,
            xticks=range(len(CUTOFFS)),
            xticklabels=[f"@{cutoff}" for cutoff in CUTOFFS],
            ylim=(0, 1) if metric != "DCG" else (0, None),
        )
        axis.set_axisbelow(True)
        axis.grid(axis="y", alpha=0.22)
    handles, _ = axes[0, 0].get_legend_handles_labels()
    figure.legend(handles, names, ncols=columns, loc="outside lower center")
    save(figure, output / "metrics.png")

    figure, axis = plt.subplots(
        figsize=(
            max(7, min(20, 3 + len(query_ids) * 0.38)),
            max(3, 2 + len(names) * 0.5),
        ),
        layout="constrained",
    )
    image = axis.imshow(per_query, aspect="auto", cmap="viridis", vmin=0, vmax=1)
    axis.set(
        title="NDCG@5 по запросам",
        xlabel="Запрос",
        xticks=range(len(query_ids)),
        yticks=range(len(names)),
        yticklabels=names,
    )
    axis.set_xticklabels(query_ids, rotation=45, ha="right")
    figure.colorbar(image, ax=axis, label="NDCG@5")
    save(figure, output / "ndcg-by-query.png")


def plot_coverage(runs, qrels, output):
    with qrels.open(encoding="utf-8", newline="") as stream:
        judged = {
            (query, url)
            for query, url, _ in csv.reader(
                stream, delimiter="\t", quoting=csv.QUOTE_NONE
            )
        }

    totals = {}
    with runs.open(encoding="utf-8", newline="") as stream:
        for engine, query, rank, url in csv.reader(
            stream, delimiter="\t", quoting=csv.QUOTE_NONE
        ):
            counts = totals.setdefault(engine, [0, 0])
            if rank != "0":
                counts[0] += (query, url) in judged
                counts[1] += 1
    if not totals:
        raise ValueError("В runs.tsv нет систем")

    figure, axis = plt.subplots(
        figsize=(9, max(2.8, 1.6 + len(totals) * 0.48)), layout="constrained"
    )
    positions = range(len(totals))
    shares = [covered / total if total else 0 for covered, total in totals.values()]
    axis.barh(positions, [1] * len(totals), height=0.6, color="#ebeef2")
    axis.barh(positions, shares, height=0.6, color="#2b718a")
    axis.set(
        title="Покрытие результатов оценками",
        xlabel="Доля оценённых результатов",
        xlim=(0, 1),
        yticks=positions,
        yticklabels=list(totals),
    )
    axis.set_ylim((1, -1) if len(totals) == 1 else (len(totals) - 0.45, -0.55))
    axis.spines[["top", "right", "left"]].set_visible(False)
    axis.tick_params(axis="y", length=0, pad=8)
    axis.xaxis.set_major_formatter(PercentFormatter(xmax=1))
    for i, (covered, total) in enumerate(totals.values()):
        axis.text(
            0.98,
            i,
            f"{covered} из {total}" if total else "пустая выдача",
            ha="right",
            va="center",
            color="#192532",
            bbox={"facecolor": "white", "edgecolor": "none", "pad": 2},
        )
    output.mkdir(parents=True, exist_ok=True)
    save(figure, output / "coverage.png")


def main():
    parser = argparse.ArgumentParser(description="Графики оценки качества поиска")
    commands = parser.add_subparsers(dest="command", required=True)
    metrics = commands.add_parser("metrics")
    metrics.add_argument("source", type=Path)
    metrics.add_argument("output", type=Path)
    coverage = commands.add_parser("coverage")
    coverage.add_argument("runs", type=Path)
    coverage.add_argument("qrels", type=Path)
    coverage.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "metrics":
            plot_metrics(args.source, args.output)
        else:
            plot_coverage(args.runs, args.qrels, args.output)
    except (KeyError, OSError, TypeError, ValueError) as error:
        parser.exit(1, f"Не удалось построить графики: {error}\n")


if __name__ == "__main__":
    main()
