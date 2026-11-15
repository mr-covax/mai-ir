import argparse
import json
import struct
from collections import defaultdict
from hashlib import sha256
from pathlib import Path
from tempfile import TemporaryDirectory
from time import time

from pymongo import MongoClient

from tokenizer.extraction import extract_document


def export(uri, output):
    output = output.absolute()
    if output.exists():
        raise ValueError(f"Output exists: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    totals = defaultdict(lambda: dict(documents=0, raw_bytes=0, text_bytes=0))
    with MongoClient(uri, serverSelectionTimeoutMS=5000) as client:
        database = client.get_default_database()
        if "documents" not in database.list_collection_names():
            raise ValueError("Crawler documents collection does not exist")
        with (
            client.start_session(snapshot=True) as session,
            TemporaryDirectory(dir=output.parent) as temporary,
        ):
            root = Path(temporary)
            (root / "texts").mkdir()
            (root / "raw").mkdir()
            with (
                (root / "manifest.jsonl").open("w", encoding="utf-8") as manifest,
                (root / "corpus.bin").open("wb") as stream,
            ):
                stream.write(b"IRDOC001")
                for row in database.documents.find({}, session=session).sort("url", 1):
                    url, source, html = row["url"], row["source"], row["html"]
                    document = extract_document(html, source)
                    identifier = sha256(url.encode()).hexdigest()
                    raw = html.encode("utf-8")
                    text = (document.text + "\n").encode("utf-8")
                    encoded_url, title = url.encode("utf-8"), document.title.encode("utf-8")
                    if b"\0" in encoded_url + title + text:
                        raise ValueError(f"NUL in document: {url}")
                    (root / "raw" / f"{identifier}.html").write_bytes(raw)
                    path = f"texts/{identifier}.txt"
                    (root / path).write_bytes(text)
                    entry = dict(
                        id=identifier,
                        url=url,
                        source=source,
                        title=document.title,
                        path=path,
                        text_bytes=len(text),
                    )
                    manifest.write(json.dumps(entry, ensure_ascii=False) + "\n")
                    stream.write(struct.pack("<IIQ", len(encoded_url), len(title), len(text)))
                    stream.write(encoded_url + title + text)
                    total = totals[source]
                    total["documents"] += 1
                    total["raw_bytes"] += len(raw)
                    total["text_bytes"] += len(text)
            total = {
                key: sum(item[key] for item in totals.values())
                for key in ("documents", "raw_bytes", "text_bytes")
            }
            for item in [total, *totals.values()]:
                n = item["documents"]
                item["average_raw_bytes"] = item["raw_bytes"] / n if n else 0
                item["average_text_bytes"] = item["text_bytes"] / n if n else 0
            report = dict(
                exported_at=int(time()), database=database.name, sources=dict(totals), total=total
            )
            (root / "statistics.json").write_text(
                json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
            )
            if output.exists():
                raise ValueError(f"Output exists: {output}")
            root.rename(output)
    print(json.dumps(report, ensure_ascii=False))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("uri")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    export(args.uri, args.output)
