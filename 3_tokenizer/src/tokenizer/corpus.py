import math
import json
import subprocess
from hashlib import sha256
from itertools import islice
from pathlib import Path
from statistics import median
from time import perf_counter

from tokenizer.models import CorpusReport, CorpusRun, ManifestEntry, TokenStatistics


def tokenize_file(binary: Path, path: Path, expected_bytes: int, timeout: float) -> TokenStatistics:
    if path.stat().st_size != expected_bytes:
        raise ValueError(f"File size does not match manifest: {path}")

    try:
        result = subprocess.run(
            [str(binary), "--stats-only", str(path)],
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as error:
        raise ValueError(f"Tokenizer timed out for {path}") from error

    if result.returncode != 0:
        raise ValueError(
            f"Tokenizer failed for {path}: {result.stderr.strip() or result.returncode}"
        )

    try:
        stats = TokenStatistics.model_validate_json(result.stdout)
    except ValueError as error:
        raise ValueError(f"Invalid tokenizer response for {path}: {error}") from error

    if stats.input_bytes != expected_bytes:
        raise ValueError(f"Tokenizer input size does not match manifest: {path}")

    average = stats.total_length / stats.tokens if stats.tokens else 0

    if abs(stats.average_length - average) > 1e-9 or (not stats.tokens and stats.total_length):
        raise ValueError(f"Inconsistent tokenizer statistics for {path}")

    return stats


def run_stream(root: Path, binary: Path, limit: int | None, timeout: float) -> CorpusRun:
    native = binary.parent / "tokenize_stream"
    if not native.is_file():
        raise ValueError("Packed corpus requires tokenize_stream next to the tokenizer binary")
    command = [str(native), str(root / "corpus.bin")]
    if limit is not None:
        command.append(str(limit))
    try:
        result = subprocess.run(
            command, capture_output=True, text=True, encoding="utf-8", timeout=timeout
        )
    except subprocess.TimeoutExpired as error:
        raise ValueError(
            "Corpus tokenizer timed out; increase --timeout for a large packed corpus"
        ) from error
    if result.returncode:
        raise ValueError(f"Corpus tokenizer failed: {result.stderr.strip()}")
    data = json.loads(result.stdout)
    documents = 0
    size = 0
    identifiers = set()
    previous = 8
    corpus_size = (root / "corpus.bin").stat().st_size
    with (root / "manifest.jsonl").open(encoding="utf-8") as manifest:
        for line in islice(manifest, limit):
            entry = ManifestEntry.model_validate_json(line)
            if entry.id != sha256(entry.url.encode()).hexdigest() or entry.id in identifiers:
                raise ValueError("Invalid or duplicate corpus identity")
            if (
                entry.path != "corpus.bin"
                or entry.text_offset is None
                or entry.text_offset < previous
                or entry.text_offset + entry.text_bytes > corpus_size
            ):
                raise ValueError("Invalid packed corpus offset")
            previous = entry.text_offset + entry.text_bytes
            identifiers.add(entry.id)
            documents += 1
            size += entry.text_bytes
    if data["documents"] != documents or data["input_bytes"] != size:
        raise ValueError("Corpus stream differs from manifest")
    return CorpusRun(
        documents=documents,
        input_bytes=size,
        tokens=data["tokens"],
        total_length=data["total_length"],
        average_length=data["average_length"],
        tokenization_seconds=data["tokenization_seconds"],
        wall_seconds=data["wall_seconds"],
        overhead_seconds=max(0, data["wall_seconds"] - data["tokenization_seconds"]),
        seconds_per_kib=data["seconds_per_kib"],
        mib_per_second=data["mib_per_second"],
    )


def run_corpus(root: Path, binary: Path, limit: int | None, timeout: float) -> CorpusRun:
    if (root / "corpus.bin").is_file() and (binary.parent / "tokenize_stream").is_file():
        with (root / "manifest.jsonl").open(encoding="utf-8") as manifest:
            first = manifest.readline()
        if first and ManifestEntry.model_validate_json(first).text_offset is not None:
            return run_stream(root, binary, limit, timeout)
    started = perf_counter()
    documents = 0
    tokens = 0
    total_length = 0
    input_bytes = 0
    seconds = 0.0
    identifiers = set()
    paths = set()

    with (root / "manifest.jsonl").open(encoding="utf-8") as manifest:
        for number, line in enumerate(islice(manifest, limit), start=1):
            try:
                entry = ManifestEntry.model_validate_json(line)
            except ValueError as error:
                raise ValueError(f"Invalid manifest entry at line {number}: {error}") from error

            identifier = sha256(entry.url.encode("utf-8")).hexdigest()
            relative = Path(entry.path)
            path = (root / relative).resolve()

            if relative.is_absolute() or not path.is_relative_to(root):
                raise ValueError(f"Document path leaves corpus directory: {entry.path}")

            if entry.id != identifier or entry.id in identifiers or path in paths:
                raise ValueError(f"Invalid or duplicate document identity at line {number}")

            if not path.is_file():
                raise ValueError(f"Document is not a file: {path}")

            identifiers.add(entry.id)
            paths.add(path)
            stats = tokenize_file(binary, path, entry.text_bytes, timeout)
            documents += 1
            tokens += stats.tokens
            total_length += stats.total_length
            input_bytes += stats.input_bytes
            seconds += stats.elapsed_seconds

    wall_seconds = perf_counter() - started

    return CorpusRun(
        documents=documents,
        tokens=tokens,
        total_length=total_length,
        average_length=total_length / tokens if tokens else 0,
        input_bytes=input_bytes,
        tokenization_seconds=seconds,
        wall_seconds=wall_seconds,
        overhead_seconds=max(0, wall_seconds - seconds),
        seconds_per_kib=seconds * 1024 / input_bytes if input_bytes else 0,
        mib_per_second=input_bytes / (seconds * 1024 * 1024) if seconds else 0,
    )


def tokenize_corpus(
    root: Path,
    binary: Path,
    *,
    limit: int | None = None,
    repeat: int = 1,
    timeout: float = 60,
) -> CorpusReport:
    if limit is not None and limit < 0:
        raise ValueError("Limit must be non-negative")

    if repeat < 1:
        raise ValueError("Repeat count must be positive")

    if timeout <= 0 or not math.isfinite(timeout):
        raise ValueError("Timeout must be finite and positive")

    root = root.resolve(strict=True)
    binary = binary.resolve(strict=True)
    runs = tuple(run_corpus(root, binary, limit, timeout) for _ in range(repeat))
    reference = runs[0]

    for run in runs[1:]:
        if (run.documents, run.tokens, run.total_length, run.input_bytes) != (
            reference.documents,
            reference.tokens,
            reference.total_length,
            reference.input_bytes,
        ):
            raise ValueError("Tokenization results differ between runs")

    return CorpusReport(
        runs=runs,
        median_tokenization_seconds=median(run.tokenization_seconds for run in runs),
        median_wall_seconds=median(run.wall_seconds for run in runs),
    )
