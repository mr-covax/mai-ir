import argparse
import gzip
import hashlib
import html
import json
import multiprocessing
import os
import sqlite3
import struct
import time
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor
from pathlib import Path
from urllib.parse import urlsplit, urlunsplit

import httpx
import pyarrow.parquet as parquet
from lxml import html as lxml_html

from tokenizer.extraction import extract_document


DATASET = "HuggingFaceH4/stack-exchange-preferences"
REVISION = "c7bda74048748f55749cd663c3d8d1025a841fd9"
SITES = (
    "softwareengineering.stackexchange.com",
    "codereview.stackexchange.com",
    "unix.stackexchange.com",
    "Stackoverflow.com",
    "cs.stackexchange.com",
    "dba.stackexchange.com",
    "gamedev.stackexchange.com",
)


def dump(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def transform(arguments):
    row, minimum = arguments
    bodies = [row["question"], *(answer["text"] for answer in row["answers"])]
    raw = "\n".join(bodies)
    if len(raw.split()) < minimum:
        return None
    question = lxml_html.fragment_fromstring(row["question"], create_parent="div")
    title = " ".join(question.text_content().split())[:180]
    rendered = (
        f"<article><h1>{html.escape(title)}</h1>"
        + "".join(f"<section>{body}</section>" for body in bodies)
        + "</article>"
    )
    document = extract_document(rendered, "stackexchange")
    text = document.text + "\n"
    words = len(text.split())
    if words < minimum or "\0" in text or len(text.encode()) > 64 * 1024 * 1024:
        return None
    url = urlsplit(row["metadata"][0])
    normalized = urlunsplit(("https", url.netloc.lower(), url.path.rstrip("/"), "", ""))
    return dict(
        url=normalized,
        source=url.netloc.lower(),
        title=title,
        text=text,
        words=words,
        raw_bytes=sum(len(body.encode()) for body in bodies),
    )


def download(client, row, cache):
    path = cache / row["path"].replace("/", "_")
    expected = row.get("lfs", {}).get("oid")
    if path.exists() and path.stat().st_size == row["size"]:
        if not expected or hashlib.file_digest(path.open("rb"), "sha256").hexdigest() == expected:
            return path
    temporary = path.with_suffix(".partial")
    for attempt in range(5):
        try:
            url = f"https://huggingface.co/datasets/{DATASET}/resolve/{REVISION}/{row['path']}?download=true&attempt={attempt}"
            with client.stream("GET", url) as response, temporary.open("wb") as stream:
                response.raise_for_status()
                digest = hashlib.sha256()
                for chunk in response.iter_bytes(1024 * 1024):
                    stream.write(chunk)
                    digest.update(chunk)
            if temporary.stat().st_size != row["size"] or (
                expected and digest.hexdigest() != expected
            ):
                raise ValueError("Shard checksum differs from upstream metadata")
            temporary.replace(path)
            return path
        except (OSError, ValueError, httpx.HTTPError):
            if attempt == 4:
                raise
            time.sleep(min(2**attempt, 16))
    raise RuntimeError("Download failed")


def accept(root):
    """Truncate partially written data to the checkpoint and record the accepted corpus."""
    state = json.loads((root / "checkpoint.json").read_text())
    with (root / "corpus.bin").open("r+b") as stream:
        stream.truncate(state["stream_bytes"])
    with (root / "manifest.jsonl").open("r+b") as manifest:
        manifest.truncate(state["manifest_bytes"])
    with sqlite3.connect(root / "identities.sqlite") as database:
        database.execute("DELETE FROM documents WHERE ordinal >= ?", (state["documents"],))
        count = database.execute("SELECT count(*) FROM documents").fetchone()[0]
        if count != state["documents"]:
            raise ValueError("Identity database differs from checkpoint")
    report = dict(
        exported_at=int(time.time()),
        dataset=state["dataset"],
        revision=state["revision"],
        format="packed-v2",
        sources=state["sources"],
        total={key: state[key] for key in ("documents", "raw_bytes", "text_bytes", "words")},
        target_met=state["documents"] >= state["target"],
        requested_target=state["target"],
        stop_reason="User accepted the collected corpus without the million-document requirement",
        minimum_words_filter=state["filter_minimum_words"],
        minimum_document_words=state["minimum_words"],
        maximum_document_words=state["maximum_words"],
    )
    for item in [report["total"], *report["sources"].values()]:
        count = item["documents"]
        for kind in ("raw_bytes", "text_bytes", "words"):
            item[f"average_{kind}"] = item[kind] / count if count else 0
    dump(root / "statistics.json", report)
    print(json.dumps(report, ensure_ascii=False))


def collect(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "raw").mkdir(exist_ok=True)
    args.cache.mkdir(parents=True, exist_ok=True)
    checkpoint = output / "checkpoint.json"
    state = (
        json.loads(checkpoint.read_text())
        if checkpoint.exists()
        else dict(
            documents=0,
            text_bytes=0,
            raw_bytes=0,
            words=0,
            minimum_words=None,
            maximum_words=0,
            sources={},
            completed=[],
            stream_bytes=8,
            manifest_bytes=0,
            target=args.target,
            filter_minimum_words=args.minimum_words,
            dataset=DATASET,
            revision=REVISION,
        )
    )
    if (
        state["revision"] != REVISION
        or state["filter_minimum_words"] != args.minimum_words
        or state["target"] != args.target
    ):
        raise ValueError("Import settings differ from existing checkpoint")
    database = sqlite3.connect(output / "identities.sqlite")
    database.execute(
        "CREATE TABLE IF NOT EXISTS documents(url TEXT PRIMARY KEY, content_hash TEXT UNIQUE, ordinal INTEGER NOT NULL)"
    )
    database.execute("CREATE INDEX IF NOT EXISTS ordinal_idx ON documents(ordinal)")
    database.execute("DELETE FROM documents WHERE ordinal >= ?", (state["documents"],))
    database.commit()
    stream_path, manifest_path = output / "corpus.bin", output / "manifest.jsonl"
    if not stream_path.exists():
        stream_path.write_bytes(b"IRDOC001")
    manifest_path.touch(exist_ok=True)
    totals = defaultdict(
        lambda: dict(documents=0, raw_bytes=0, text_bytes=0, words=0), state["sources"]
    )
    with stream_path.open("r+b") as stream, manifest_path.open("r+b") as manifest:
        stream.truncate(state["stream_bytes"])
        stream.seek(state["stream_bytes"])
        manifest.truncate(state["manifest_bytes"])
        manifest.seek(state["manifest_bytes"])

        def add(document):
            text = document["text"].encode("utf-8")
            url, title = document["url"].encode(), document["title"].encode()
            if b"\0" in text + url + title:
                raise ValueError("NUL in document")
            digest = hashlib.sha256(text).hexdigest()
            try:
                database.execute(
                    "INSERT INTO documents VALUES (?,?,?)",
                    (document["url"], digest, state["documents"]),
                )
            except sqlite3.IntegrityError:
                return False
            stream.write(struct.pack("<IIQ", len(url), len(title), len(text)))
            stream.write(url + title)
            offset = stream.tell()
            stream.write(text)
            identifier = hashlib.sha256(url).hexdigest()
            entry = dict(
                id=identifier,
                url=document["url"],
                source=document["source"],
                title=document["title"],
                path="corpus.bin",
                text_bytes=len(text),
                text_offset=offset,
            )
            manifest.write((json.dumps(entry, ensure_ascii=False) + "\n").encode())
            state["documents"] += 1
            state["text_bytes"] += len(text)
            state["raw_bytes"] += document["raw_bytes"]
            state["words"] += document["words"]
            minimum = state["minimum_words"]
            state["minimum_words"] = (
                min(minimum, document["words"]) if minimum is not None else document["words"]
            )
            state["maximum_words"] = max(state["maximum_words"], document["words"])
            item = totals[document["source"]]
            item["documents"] += 1
            item["raw_bytes"] += document["raw_bytes"]
            item["text_bytes"] += len(text)
            item["words"] += document["words"]
            return True

        def commit(label):
            stream.flush()
            manifest.flush()
            os.fsync(stream.fileno())
            os.fsync(manifest.fileno())
            state["stream_bytes"] = stream.tell()
            state["manifest_bytes"] = manifest.tell()
            state["sources"] = dict(totals)
            state["completed"].append(label)
            database.commit()
            dump(checkpoint, state)
            report = {
                key: state[key]
                for key in (
                    "documents",
                    "text_bytes",
                    "raw_bytes",
                    "words",
                    "minimum_words",
                    "maximum_words",
                    "sources",
                )
            }
            report.update(
                average_words=state["words"] / state["documents"] if state["documents"] else 0,
                target_met=state["documents"] >= args.target,
                completed_shards=len(state["completed"]),
                revision=REVISION,
            )
            dump(output / "progress.json", report)
            print(
                json.dumps(
                    dict(
                        event="checkpoint",
                        label=label,
                        documents=state["documents"],
                        text_bytes=state["text_bytes"],
                    )
                ),
                flush=True,
            )

        if args.base and "base" not in state["completed"]:
            with gzip.open(output / "raw/base.jsonl.gz", "wt", encoding="utf-8") as raw:
                for line in (args.base / "manifest.jsonl").read_text(encoding="utf-8").splitlines():
                    entry = json.loads(line)
                    text = (args.base / entry["path"]).read_text(encoding="utf-8")
                    if len(text.split()) < 200:
                        continue
                    original = (args.base / "raw" / f"{entry['id']}.html").read_text(
                        encoding="utf-8"
                    )
                    document = dict(
                        url=entry["url"],
                        title=entry["title"],
                        source=entry["source"],
                        text=text,
                        words=len(text.split()),
                        raw_bytes=len(original.encode()),
                    )
                    if add(document):
                        raw.write(
                            json.dumps(
                                dict(url=entry["url"], html=original, source=entry["source"]),
                                ensure_ascii=False,
                            )
                            + "\n"
                        )
            commit("base")

        with httpx.Client(
            follow_redirects=True,
            timeout=httpx.Timeout(180, connect=30),
            headers={"User-Agent": "MAI-IR-Corpus/0.2"},
        ) as client:
            rows = []
            for site in SITES:
                response = client.get(
                    f"https://huggingface.co/api/datasets/{DATASET}/tree/{REVISION}/data/{site}",
                    params=dict(limit=1000),
                )
                response.raise_for_status()
                rows.extend(
                    sorted(
                        (
                            row
                            for row in response.json()
                            if row["type"] == "file" and row["path"].endswith(".parquet")
                        ),
                        key=lambda row: row["path"],
                    )
                )
            dump(
                output / "provenance.json",
                dict(
                    dataset=DATASET,
                    revision=REVISION,
                    source_url=f"https://huggingface.co/datasets/{DATASET}",
                    license="CC-BY-SA-4.0 as declared by source dataset",
                    sites=SITES,
                    shards=rows,
                    document_unit="one distinct question thread including all provided answers",
                    title="first 180 normalized characters of question, generated locally",
                ),
            )
            pending = [row for row in rows if row["path"] not in state["completed"]]
            with (
                ThreadPoolExecutor(max_workers=2) as downloads,
                ProcessPoolExecutor(
                    max_workers=args.workers, mp_context=multiprocessing.get_context("spawn")
                ) as workers,
            ):
                future = (
                    downloads.submit(download, client, pending[0], args.cache) if pending else None
                )
                for number, row in enumerate(pending):
                    if state["documents"] >= args.target:
                        break
                    shard = future.result()
                    future = (
                        downloads.submit(download, client, pending[number + 1], args.cache)
                        if number + 1 < len(pending)
                        else None
                    )
                    raw_name = row["path"].replace("/", "_").replace(".parquet", ".jsonl.gz")
                    before = state["documents"]
                    with gzip.open(
                        output / "raw" / raw_name, "wt", encoding="utf-8", compresslevel=3
                    ) as raw:
                        file = parquet.ParquetFile(shard)
                        for batch in file.iter_batches(batch_size=128):
                            batch_rows = batch.to_pylist()
                            for original, document in zip(
                                batch_rows,
                                workers.map(
                                    transform,
                                    ((item, args.minimum_words) for item in batch_rows),
                                    chunksize=8,
                                ),
                            ):
                                if document and add(document):
                                    raw.write(json.dumps(original, ensure_ascii=False) + "\n")
                                if state["documents"] >= args.target:
                                    break
                            if state["documents"] >= args.target:
                                break
                    row["download_sha256"] = hashlib.file_digest(
                        shard.open("rb"), "sha256"
                    ).hexdigest()
                    commit(row["path"])
                    print(
                        json.dumps(
                            dict(
                                event="shard",
                                accepted=state["documents"] - before,
                                file=row["path"],
                            )
                        ),
                        flush=True,
                    )
                    shard.unlink()
                if future:
                    future.result()
    database.close()
    report = dict(
        exported_at=int(time.time()),
        dataset=DATASET,
        revision=REVISION,
        format="packed-v2",
        sources=dict(totals),
        total={key: state[key] for key in ("documents", "raw_bytes", "text_bytes", "words")},
        target_met=state["documents"] >= args.target,
        minimum_words_filter=args.minimum_words,
        minimum_document_words=state["minimum_words"],
        maximum_document_words=state["maximum_words"],
    )
    for item in [report["total"], *report["sources"].values()]:
        count = item["documents"]
        for kind in ("raw_bytes", "text_bytes", "words"):
            item[f"average_{kind}"] = item[kind] / count if count else 0
    dump(output / "statistics.json", report)
    if report["target_met"]:
        return 0
    if args.accept:
        print(json.dumps(dict(event="accept", documents=state["documents"]), flush=True))
        accept(output)
        return 0
    return 2


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--base", type=Path)
    parser.add_argument("--cache", type=Path, default=Path("data/stackexchange-cache"))
    parser.add_argument("--target", type=int, default=1000001)
    parser.add_argument("--minimum-words", type=int, default=1000)
    parser.add_argument("--workers", type=int, default=6)
    parser.add_argument(
        "--accept",
        action="store_true",
        help="Accept the corpus as collected when the target is not reached",
    )
    args = parser.parse_args()
    if args.target < 1 or args.minimum_words < 1 or args.workers < 1:
        parser.error("Counts must be positive")
    raise SystemExit(collect(args))
