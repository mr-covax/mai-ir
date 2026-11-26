import argparse
import json
from configparser import ConfigParser
from pathlib import Path


def summarize(root):
    result = {}

    for group in ("all", "rare", "medium", "common"):
        raw = json.loads((root / f"raw-{group}.json").read_text())
        compressed = json.loads((root / f"vb-{group}.json").read_text())

        for field in ("terms", "postings", "positions", "checksum"):
            if raw[field] != compressed[field]:
                raise ValueError(f"raw and vb differ in {group}.{field}")

        result[group] = dict(
            raw=raw,
            compressed=compressed,
            size_ratio=compressed["coordinate_bytes"] / raw["coordinate_bytes"]
            if raw["coordinate_bytes"]
            else 0,
            scan_time_ratio=compressed["elapsed_seconds"] / raw["elapsed_seconds"]
            if raw["elapsed_seconds"]
            else 0,
        )

    raw_stats = json.loads((root / "raw-statistics.json").read_text())
    vb_stats = json.loads((root / "vb-statistics.json").read_text())

    for field in ("documents", "terms", "positions", "postings"):
        if raw_stats[field] != vb_stats[field]:
            raise ValueError(f"raw and vb differ in statistics.{field}")

    raw_search = json.loads((root / "raw-search.json").read_text())
    vb_search = json.loads((root / "vb-search.json").read_text())

    if raw_search["result_checksum"] != vb_search["result_checksum"]:
        raise ValueError("raw and vb search results differ")

    (root / "summary.json").write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("results", type=Path, nargs="?")
    args = parser.parse_args()
    if args.results is None:
        config = ConfigParser(interpolation=None)
        config.read_string("[lab]\n" + Path("config").read_text(encoding="utf-8"))
        args.results = Path(config["lab"]["experiment_output"].strip('"'))
    summarize(args.results)
