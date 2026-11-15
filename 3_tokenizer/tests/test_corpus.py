import json
import subprocess
from hashlib import sha256

import pytest

from tokenizer.corpus import tokenize_corpus
from tokenizer.models import ManifestEntry


@pytest.fixture
def corpus(tmp_path):
    root = tmp_path / "corpus"
    (root / "texts").mkdir(parents=True)
    entries = []

    for name, text in [("first", "a a a\n"), ("second", "bbbbbbbb\n")]:
        url = f"https://example.org/{name}"
        identifier = sha256(url.encode()).hexdigest()
        path = f"texts/{identifier}.txt"
        data = text.encode()
        (root / path).write_bytes(data)
        entries.append(
            ManifestEntry(
                id=identifier,
                url=url,
                source="linux-kernel",
                title=name,
                path=path,
                text_bytes=len(data),
            )
        )

    (root / "manifest.jsonl").write_text(
        "".join(entry.model_dump_json() + "\n" for entry in entries)
    )
    return root


def test_aggregate_statistics(corpus, executable):
    report = tokenize_corpus(corpus, executable, repeat=3)

    assert len(report.runs) == 3

    for run in report.runs:
        assert run.documents == 2
        assert run.tokens == 4
        assert run.total_length == 11
        assert run.average_length == 2.75
        assert run.input_bytes == 15
        assert run.tokenization_seconds >= 0
        assert run.wall_seconds >= run.tokenization_seconds
        assert run.overhead_seconds == pytest.approx(run.wall_seconds - run.tokenization_seconds)
        assert run.seconds_per_kib == pytest.approx(run.tokenization_seconds * 1024 / 15)

    assert (
        report.median_tokenization_seconds
        == sorted(run.tokenization_seconds for run in report.runs)[1]
    )


def test_limit(corpus, executable):
    run = tokenize_corpus(corpus, executable, limit=1).runs[0]

    assert run.documents == 1
    assert run.tokens == 3
    assert run.total_length == 3
    assert run.input_bytes == 6


@pytest.mark.parametrize("empty_manifest", [False, True])
def test_empty_selection(corpus, executable, empty_manifest):
    if empty_manifest:
        (corpus / "manifest.jsonl").write_text("")

    run = tokenize_corpus(corpus, executable, limit=None if empty_manifest else 0).runs[0]

    assert run.documents == 0
    assert run.tokens == 0
    assert run.average_length == 0
    assert run.seconds_per_kib == 0
    assert run.mib_per_second == 0


@pytest.mark.parametrize(
    "options",
    [
        {"limit": -1},
        {"repeat": 0},
        {"timeout": 0},
        {"timeout": float("nan")},
        {"timeout": float("inf")},
    ],
)
def test_invalid_options(corpus, executable, options):
    with pytest.raises(ValueError):
        tokenize_corpus(corpus, executable, **options)


def test_missing_document(corpus, executable):
    next((corpus / "texts").iterdir()).unlink()

    with pytest.raises(ValueError, match="not a file"):
        tokenize_corpus(corpus, executable)


def test_size_mismatch(corpus, executable):
    next((corpus / "texts").iterdir()).write_text("Changed")

    with pytest.raises(ValueError, match="size does not match"):
        tokenize_corpus(corpus, executable)


def test_duplicate_entries(corpus, executable):
    manifest = corpus / "manifest.jsonl"
    manifest.write_text(manifest.read_text() * 2)

    with pytest.raises(ValueError, match="duplicate"):
        tokenize_corpus(corpus, executable)


def test_invalid_json(corpus, executable):
    (corpus / "manifest.jsonl").write_text("not json\n")

    with pytest.raises(ValueError, match="line 1"):
        tokenize_corpus(corpus, executable)


def test_path_traversal(corpus, executable):
    manifest = corpus / "manifest.jsonl"
    entry = json.loads(manifest.read_text().splitlines()[0])
    entry["path"] = "../outside.txt"
    manifest.write_text(json.dumps(entry))

    with pytest.raises(ValueError, match="leaves corpus"):
        tokenize_corpus(corpus, executable)


def test_symlink_outside_corpus(corpus, executable, tmp_path):
    path = next((corpus / "texts").iterdir())
    outside = tmp_path / "outside.txt"
    outside.write_bytes(path.read_bytes())
    path.unlink()
    path.symlink_to(outside)

    with pytest.raises(ValueError, match="leaves corpus"):
        tokenize_corpus(corpus, executable)


def test_tokenizer_failure(corpus, executable, monkeypatch):
    def fail(*args, **kwargs):
        return subprocess.CompletedProcess(args[0], 1, "", "Invalid UTF-8")

    monkeypatch.setattr("tokenizer.corpus.subprocess.run", fail)

    with pytest.raises(ValueError, match="Invalid UTF-8"):
        tokenize_corpus(corpus, executable)


def test_invalid_tokenizer_response(corpus, executable, monkeypatch):
    def respond(*args, **kwargs):
        return subprocess.CompletedProcess(args[0], 0, "not json", "")

    monkeypatch.setattr("tokenizer.corpus.subprocess.run", respond)

    with pytest.raises(ValueError, match="Invalid tokenizer response"):
        tokenize_corpus(corpus, executable)


def test_timeout(corpus, executable, monkeypatch):
    def timeout(*args, **kwargs):
        raise subprocess.TimeoutExpired(args[0], kwargs["timeout"])

    monkeypatch.setattr("tokenizer.corpus.subprocess.run", timeout)

    with pytest.raises(ValueError, match="timed out"):
        tokenize_corpus(corpus, executable)
