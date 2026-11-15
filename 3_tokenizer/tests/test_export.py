import os
from uuid import uuid4

from pymongo import MongoClient
from pymongo.errors import ConfigurationError
from hashlib import sha256

import pytest

from tokenizer.export import export_corpus
from tokenizer.models import ManifestEntry


@pytest.fixture
def database():
    uri = os.environ.get(
        "MONGODB_URI", "mongodb://localhost:27017/?replicaSet=rs0&directConnection=true"
    )
    name = "tokenizer_test_" + uuid4().hex
    with MongoClient(uri, serverSelectionTimeoutMS=5000) as client:
        client[name].documents.insert_many(
            [
                {
                    "url": "https://example.org/z",
                    "source": "linux-kernel",
                    "html": '<div role="main"><h1>Память</h1><p>malloc()</p></div>',
                },
                {
                    "url": "https://example.org/a",
                    "source": "glibc",
                    "html": "<body><h1>Threads</h1><pre>    pthread_join();</pre></body>",
                },
            ]
        )
        # Preserve credentials, hosts and connection options from MONGODB_URI.
        base, separator, options = uri.partition("?")
        authority = base.split("://", 1)[1].split("/", 1)[0]
        database_uri = base.split("://", 1)[0] + "://" + authority + "/" + name
        database_uri += separator + options if separator else ""
        try:
            yield database_uri
        finally:
            client.drop_database(name)


def documents(uri):
    with MongoClient(uri) as client:
        return list(client.get_default_database().documents.find().sort("url", 1))


def read_manifest(output):
    return [
        ManifestEntry.model_validate_json(line)
        for line in (output / "manifest.jsonl").read_text().splitlines()
    ]


def test_export(database, tmp_path):
    output = tmp_path / "corpus"
    before = documents(database)
    stats = export_corpus(database, output)
    entries = read_manifest(output)

    assert stats.documents == 2
    assert [entry.url for entry in entries] == ["https://example.org/a", "https://example.org/z"]
    assert [entry.source for entry in entries] == ["glibc", "linux-kernel"]
    assert [entry.title for entry in entries] == ["Threads", "Память"]

    for entry in entries:
        assert entry.id == sha256(entry.url.encode()).hexdigest()
        assert entry.path == f"texts/{entry.id}.txt"
        assert len((output / entry.path).read_bytes()) == entry.text_bytes

    assert (output / entries[1].path).read_text() == "Память\n\nmalloc()\n"
    assert stats.text_bytes == sum(entry.text_bytes for entry in entries)
    assert documents(database) == before


def test_source_filter(database, tmp_path):
    output = tmp_path / "corpus"
    stats = export_corpus(database, output, sources=("linux-kernel",))

    assert stats.documents == 1
    assert read_manifest(output)[0].source == "linux-kernel"


def test_empty_selection(database, tmp_path):
    output = tmp_path / "corpus"
    stats = export_corpus(database, output, sources=("clang",))

    assert stats.documents == 0
    assert stats.text_bytes == 0
    assert read_manifest(output) == []
    assert list((output / "texts").iterdir()) == []


def test_export_is_reproducible(database, tmp_path):
    first = tmp_path / "first"
    second = tmp_path / "second"
    export_corpus(database, first)
    export_corpus(database, second)

    assert (first / "manifest.jsonl").read_bytes() == (second / "manifest.jsonl").read_bytes()

    for entry in read_manifest(first):
        assert (first / entry.path).read_bytes() == (second / entry.path).read_bytes()


def test_existing_output_is_not_modified(database, tmp_path):
    output = tmp_path / "corpus"
    output.mkdir()
    sentinel = output / "keep.txt"
    sentinel.write_text("Keep")

    with pytest.raises(FileExistsError):
        export_corpus(database, output)

    assert sentinel.read_text() == "Keep"
    assert list(output.iterdir()) == [sentinel]


def test_missing_database_is_not_created(database, tmp_path):
    with MongoClient(database) as client:
        client.drop_database(client.get_default_database().name)
    output = tmp_path / "corpus"

    with pytest.raises(ValueError, match="does not exist"):
        export_corpus(database, output)

    assert not output.exists()
    with MongoClient(database) as client:
        assert client.get_default_database().name not in client.list_database_names()


def test_failure_does_not_publish_partial_export(database, tmp_path):
    with MongoClient(database) as client:
        client.get_default_database().documents.update_one(
            {"source": "linux-kernel"},
            {"$set": {"html": "<body>Broken</body>"}},
        )

    output = tmp_path / "corpus"

    with pytest.raises(ValueError, match=r"https://example.org/z.*linux-kernel"):
        export_corpus(database, output)

    assert not output.exists()
    assert not list(tmp_path.glob(".corpus-*"))


def test_interruption_cleans_staging_directory(database, tmp_path, monkeypatch):
    def interrupt(html, source):
        raise KeyboardInterrupt

    monkeypatch.setattr("tokenizer.export.extract_document", interrupt)
    output = tmp_path / "corpus"

    with pytest.raises(KeyboardInterrupt):
        export_corpus(database, output)

    assert not output.exists()
    assert not list(tmp_path.glob(".corpus-*"))


def test_unknown_source(database, tmp_path):
    with pytest.raises(ValueError, match="Unsupported sources"):
        export_corpus(database, tmp_path / "corpus", sources=("unknown",))


def test_uri_requires_database_name(tmp_path):
    with pytest.raises(ConfigurationError, match="No default database"):
        export_corpus("mongodb://localhost:27017/", tmp_path / "corpus")
