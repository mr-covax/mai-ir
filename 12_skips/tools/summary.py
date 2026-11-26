import argparse
import json
from configparser import ConfigParser
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


def summarize(root):
    rows = []
    for stride in (0, 4, 16, 64, 128, 256):
        train = json.loads((root / f"stride-{stride}-train.json").read_text())
        test = json.loads((root / f"stride-{stride}-test.json").read_text())
        stats = json.loads((root / f"stride-{stride}-statistics.json").read_text())
        rows.append(
            dict(
                stride=stride,
                train=train,
                test=test,
                index_bytes=stats["index_bytes"],
                skip_bytes=stats["skip_bytes"],
            )
        )
    selected = min(rows, key=lambda row: row["train"]["mean_seconds"])
    baseline = rows[0]
    result = dict(
        selected_stride=selected["stride"],
        test_speedup=baseline["test"]["mean_seconds"]
        / selected["test"]["mean_seconds"],
        training_speedup=baseline["train"]["mean_seconds"]
        / selected["train"]["mean_seconds"],
        measurements=rows,
    )
    (root / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
    plt.figure(figsize=(7, 4))
    labels = [str(row["stride"]) for row in rows]
    for pool in ("train", "test"):
        plt.plot(
            labels,
            [row[pool]["mean_seconds"] * 1000 for row in rows],
            marker="o",
            label=pool,
        )
    plt.xlabel("Шаг прыжка; 0 — без прыжков")
    plt.ylabel("Среднее время запроса, мс")
    plt.legend()
    plt.grid(alpha=0.3)
    plt.tight_layout()
    plt.savefig(root / "skips.png", dpi=160)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("results", type=Path, nargs="?")
    args = parser.parse_args()
    if args.results is None:
        config = ConfigParser(interpolation=None)
        config.read_string("[lab]\n" + Path("config").read_text(encoding="utf-8"))
        args.results = Path(config["lab"]["experiment_output"].strip('"'))
    summarize(args.results)
