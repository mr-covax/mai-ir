import argparse
import gzip
import hashlib
import json
import os
import struct
import subprocess
from collections import deque
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from time import time
from urllib.parse import urlsplit

try:
    from isal.igzip_threaded import open as open_archive
    from orjson import loads as load_record
except ImportError:
    open_archive = gzip.open
    load_record = json.loads


DATASET = "common-pile/stackexchange"
REVISION = "5ec3aa2e65b7c61b7c18f1ff18ccbc4d3279a23c"
SITES = (
    "codereview.stackexchange.com",
    "softwareengineering.stackexchange.com",
    "unix.stackexchange.com",
    "dba.stackexchange.com",
    "serverfault.com",
    "askubuntu.com",
    "superuser.com",
    "stackoverflow.com",
)
RECORD = struct.Struct("<IIQ")
MAGIC = b"IRDOC001"
MAX_FIELD_BYTES = 64 * 1024**2


def matches_archive(path, entry):
    if not path.is_file() or path.stat().st_size != entry["size"]:
        return False
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest() == entry["lfs"]["sha256"]


def download(root, entry):
    name = entry["rfilename"]
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    if not matches_archive(path, entry):
        temporary = path.with_suffix(".partial")
        subprocess.run(
            [
                "curl", "--fail", "--location", "--silent", "--show-error",
                "--retry", "4", "--connect-timeout", "20", "--max-time", "900",
                "--output", str(temporary),
                f"https://huggingface.co/datasets/{DATASET}/resolve/{REVISION}/{name}?download=true",
            ],
            check=True,
        )
        if not matches_archive(temporary, entry):
            raise ValueError(f"Archive differs from upstream metadata: {path}")
        temporary.replace(path)
    return path


def base_documents(base):
    with (base / "manifest.jsonl").open("rb") as manifest:
        for line in manifest:
            entry = json.loads(line)
            original = base / "raw" / f"{entry['id']}.html"
            yield entry, (base / entry["path"]).read_bytes(), original.stat().st_size


def archive_documents(archive, minimum_words):
    minimum_line_bytes = 2 * minimum_words - 1
    with open_archive(archive, "rb") as source:
        for line in source:
            if len(line) < minimum_line_bytes:
                continue
            row = load_record(line)
            body = row["text"].strip()
            words = len(body.split())
            if words < minimum_words:
                continue
            metadata = row["metadata"]
            url = metadata["url"]
            site = metadata["site"]
            if urlsplit(url).netloc != site or site not in SITES:
                raise ValueError(f"Unexpected source: {url}")
            yield (
                dict(
                    url=url,
                    title=body.split("\n", 1)[0][:300],
                    source=site,
                    license=metadata["license"],
                    authors=metadata["authors"],
                    source_record=row["id"],
                    words=words,
                ),
                (body + "\n").encode("utf-8"),
                len(line),
            )


def write_json(path, value):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def collect(args):
    metadata_path = args.cache / "upstream.json"
    if not metadata_path.exists():
        response = subprocess.check_output([
            "curl", "--fail", "--silent", "--show-error", "--retry", "4", "--max-time", "60",
            f"https://huggingface.co/api/datasets/{DATASET}/revision/{REVISION}?blobs=true",
        ])
        args.cache.mkdir(parents=True, exist_ok=True)
        write_json(metadata_path, json.loads(response))
    metadata = json.loads(metadata_path.read_bytes())
    if metadata["id"] != DATASET or metadata["sha"] != REVISION:
        raise ValueError("Archive metadata must describe the pinned dataset revision")
    entries = [
        item
        for site in SITES
        for item in sorted(metadata["siblings"], key=lambda entry: entry["rfilename"])
        if item["rfilename"].startswith(f"{site}/documents/")
        and item["rfilename"].endswith(".jsonl.gz")
    ]
    filenames = [entry["rfilename"] for entry in entries]
    later_words = None
    if args.later_minimum:
        archive, minimum = args.later_minimum
        later_words = dict(from_archive=archive, words=int(minimum))
        if archive not in filenames or not 1 <= later_words["words"] <= args.minimum_words:
            raise ValueError("Invalid archive or word count for the later selection")

    root = args.output
    root.mkdir(parents=True, exist_ok=True)
    checkpoint = root / "checkpoint.json"
    corpus_path, manifest_path = root / "corpus.bin", root / "manifest.jsonl"
    settings = dict(
        dataset=DATASET, revision=REVISION, minimum_thread_words=args.minimum_words,
        base=str(args.base.resolve()),
    )
    if checkpoint.exists():
        state = json.loads(checkpoint.read_bytes())
        if state["settings"] != settings:
            raise ValueError("Corpus settings differ from the checkpoint")
    else:
        state = dict(
            settings=settings, completed=[], documents=0, words=0, text_bytes=0, source_bytes=0,
            corpus_bytes=len(MAGIC), manifest_bytes=0, sources={},
        )
        with corpus_path.open("xb") as stream:
            stream.write(MAGIC)
        manifest_path.touch(exist_ok=False)
        write_json(checkpoint, state)

    if state.get("later_minimum") != later_words:
        if (
            "later_minimum" in state
            or later_words is None
            or later_words["from_archive"] in state["completed"]
        ):
            raise ValueError("The later selection differs from the checkpoint")
        state["later_minimum"] = later_words

    def minimum_words_for(name):
        if later_words and filenames.index(name) >= filenames.index(later_words["from_archive"]):
            return later_words["words"]
        return args.minimum_words

    def target_met():
        return state["documents"] >= args.target_documents and state["text_bytes"] >= args.target_bytes

    urls, hashes = set(), set()
    with corpus_path.open("r+b") as corpus, manifest_path.open("r+b") as manifest:
        if corpus.read(len(MAGIC)) != MAGIC:
            raise ValueError("Invalid corpus stream")
        for file, offset in ((corpus, state["corpus_bytes"]), (manifest, state["manifest_bytes"])):
            if file.seek(0, os.SEEK_END) < offset:
                raise ValueError("Corpus files are shorter than the checkpoint")
            file.truncate(offset)
        corpus.seek(state["corpus_bytes"])
        manifest.seek(0)
        for line in manifest:
            entry = json.loads(line)
            urls.add(entry["url"])
            hashes.add(bytes.fromhex(entry["sha256"]))
        if len(urls) != state["documents"] or len(hashes) != state["documents"]:
            raise ValueError("Manifest differs from the checkpoint")

        def add_documents(records):
            for entry, text, source_bytes in records:
                url = entry["url"]
                if url in urls or not 0 < len(text) <= MAX_FIELD_BYTES or b"\0" in text:
                    continue
                digest = hashlib.sha256(text).digest()
                if digest in hashes:
                    continue
                encoded_url, title = url.encode(), entry["title"].encode()
                if b"\0" in title or b"\0" in encoded_url:
                    continue
                corpus.write(RECORD.pack(len(encoded_url), len(title), len(text)))
                corpus.write(encoded_url)
                corpus.write(title)
                text_offset = corpus.tell()
                corpus.write(text)
                words = entry["words"] if "words" in entry else len(text.decode().split())
                entry.update(
                    id=hashlib.sha256(encoded_url).hexdigest(), path="corpus.bin", text_bytes=len(text),
                    text_offset=text_offset, sha256=digest.hex(), words=words,
                )
                manifest.write((json.dumps(entry, ensure_ascii=False) + "\n").encode())
                urls.add(url)
                hashes.add(digest)
                counts = state["sources"].setdefault(
                    entry["source"], dict(documents=0, words=0, text_bytes=0, source_bytes=0)
                )
                for counter in (state, counts):
                    counter["documents"] += 1
                    counter["words"] += words
                    counter["text_bytes"] += len(text)
                    counter["source_bytes"] += source_bytes

        def commit(name):
            for file in (corpus, manifest):
                file.flush()
                os.fsync(file.fileno())
            state.update(corpus_bytes=corpus.tell(), manifest_bytes=manifest.tell())
            state["completed"].append(name)
            write_json(checkpoint, state)
            print(json.dumps(dict(shard=name, documents=state["documents"],
                                  text_bytes=state["text_bytes"])), flush=True)

        if "base" not in state["completed"]:
            add_documents(base_documents(args.base))
            commit("base")

        pending = iter(entry for entry in entries if entry["rfilename"] not in state["completed"])
        with ThreadPoolExecutor(max_workers=args.downloads) as executor:
            queue = deque()
            for _ in range(args.downloads):
                if not target_met() and (entry := next(pending, None)):
                    queue.append((entry, executor.submit(download, args.cache, entry)))
            while queue and not target_met():
                entry, task = queue.popleft()
                archive = task.result()
                if next_entry := next(pending, None):
                    queue.append((next_entry, executor.submit(download, args.cache, next_entry)))
                name = entry["rfilename"]
                add_documents(archive_documents(archive, minimum_words_for(name)))
                commit(name)
            for _, task in queue:
                task.cancel()

    totals = {key: state[key] for key in ("documents", "words", "text_bytes", "source_bytes")}
    report = dict(
        format="packed-v2", exported_at=int(time()), settings=settings,
        source_url=f"https://huggingface.co/datasets/{DATASET}",
        document_unit="One archived question thread, or one page of documentation",
        source_bytes_definition="Uncompressed archived JSON lines or original documentation HTML",
        target_met=target_met(), target_documents=args.target_documents, target_text_bytes=args.target_bytes,
        corpus_bytes=state["corpus_bytes"], total=totals, sources=state["sources"], later_minimum=later_words,
        archives=state["completed"],
    )
    for counts in (totals, *state["sources"].values()):
        for field in ("words", "text_bytes", "source_bytes"):
            counts[f"average_{field}"] = counts[field] / counts["documents"] if counts["documents"] else 0
    write_json(root / "statistics.json", report)
    return 0 if target_met() else 2


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Import technical discussion archives into an IRDOC corpus")
    parser.add_argument("output", type=Path)
    parser.add_argument("--cache", required=True, type=Path)
    parser.add_argument("--base", required=True, type=Path)
    parser.add_argument("--target-bytes", required=True, type=int)
    parser.add_argument("--target-documents", type=int, default=1_000_001)
    parser.add_argument("--minimum-words", type=int, default=1_000)
    parser.add_argument("--later-minimum", nargs=2, metavar=("ARCHIVE", "WORDS"))
    parser.add_argument("--downloads", type=int, default=3)
    args = parser.parse_args()
    if min(args.target_bytes, args.target_documents, args.minimum_words, args.downloads) < 1:
        parser.error("Counts must be positive")
    raise SystemExit(collect(args))
