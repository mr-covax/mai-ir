import argparse
import json
from collections import defaultdict
from pathlib import Path


def analyze(root):
    if (root / "statistics.json").is_file():
        report = json.loads((root / "statistics.json").read_text())
        if report.get("format") == "packed-v2":
            count = 0
            size = 0
            previous = 8
            corpus_size = (root / "corpus.bin").stat().st_size
            with (root / "manifest.jsonl").open(encoding="utf-8") as manifest:
                for line in manifest:
                    document = json.loads(line)
                    offset = document["text_offset"]
                    if (
                        document["path"] != "corpus.bin"
                        or offset < previous
                        or offset + document["text_bytes"] > corpus_size
                    ):
                        raise ValueError("Invalid packed text offset")
                    previous = offset + document["text_bytes"]
                    count += 1
                    size += document["text_bytes"]
            if (
                count != report["total"]["documents"]
                or size != report["total"]["text_bytes"]
                or previous != corpus_size
            ):
                raise ValueError("Packed corpus differs from statistics")
            print(json.dumps(report, ensure_ascii=False, indent=2))
            return
    totals = defaultdict(lambda: dict(documents=0, raw_bytes=0, text_bytes=0))
    with (root / "manifest.jsonl").open(encoding="utf-8") as manifest:
        for line in manifest:
            document = json.loads(line)
            text = (root / document["path"]).resolve()
            if not text.is_relative_to(root.resolve()):
                raise ValueError("Text path leaves corpus")
            size = text.stat().st_size
            if size != document["text_bytes"]:
                raise ValueError(f"Text size differs from manifest: {text}")
            raw = root / "raw" / f"{document['id']}.html"
            item = totals[document["source"]]
            item["documents"] += 1
            item["raw_bytes"] += raw.stat().st_size
            item["text_bytes"] += size
    total = {
        key: sum(item[key] for item in totals.values())
        for key in ("documents", "raw_bytes", "text_bytes")
    }
    for item in [total, *totals.values()]:
        count = item["documents"]
        item["average_raw_bytes"] = item["raw_bytes"] / count if count else 0
        item["average_text_bytes"] = item["text_bytes"] / count if count else 0
    print(json.dumps(dict(sources=dict(totals), total=total), ensure_ascii=False, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("corpus", type=Path)
    analyze(parser.parse_args().corpus)
