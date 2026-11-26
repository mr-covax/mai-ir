import argparse
import json
import sqlite3
import struct
import time
from pathlib import Path

import tantivy


def documents(path):
    with path.open("rb") as stream:
        if stream.read(8) != b"IRDOC001":
            raise ValueError("Invalid corpus stream")
        while header := stream.read(16):
            if len(header) != 16:
                raise ValueError("Truncated corpus header")
            a, b, size = struct.unpack("<IIQ", header)
            url = stream.read(a).decode()
            title = stream.read(b).decode()
            offset = stream.tell()
            text = stream.read(size).decode()
            yield url, title, offset, size, text


def run(corpus, queries, output):
    output.mkdir(parents=True, exist_ok=True)
    sql = sqlite3.connect(output / "sqlite.db")
    sql.executescript(
        "PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF; PRAGMA cache_size=-65536;"
    )
    existing = sql.execute(
        "SELECT count(*) FROM sqlite_master WHERE name='metadata'"
    ).fetchone()[0]
    if existing and not (output / "build.json").is_file():
        raise ValueError("Incomplete baseline indexes; use a new output directory")
    builder = tantivy.SchemaBuilder()
    builder.add_text_field("url", stored=True, tokenizer_name="raw")
    builder.add_text_field("body", stored=False)
    schema = builder.build()
    folder = output / "tantivy"
    folder.mkdir(exist_ok=True)
    index = tantivy.Index(schema, path=str(folder))
    if not existing:
        sql.execute(
            "CREATE TABLE metadata(id INTEGER PRIMARY KEY,url TEXT UNIQUE,title TEXT,offset INTEGER,bytes INTEGER)"
        )
        sql.execute(
            "CREATE VIRTUAL TABLE docs USING fts5(body, content='', tokenize='unicode61')"
        )
        writer = index.writer(heap_size=64000000, num_threads=2)
        started = time.monotonic()
        count = 0
        for count, (url, title, offset, size, text) in enumerate(
            documents(corpus / "corpus.bin"), 1
        ):
            sql.execute(
                "INSERT INTO metadata VALUES (?,?,?,?,?)",
                (count, url, title, offset, size),
            )
            sql.execute("INSERT INTO docs(rowid,body) VALUES (?,?)", (count, text))
            writer.add_document(tantivy.Document(url=url, body=text))
            if count % 10000 == 0:
                print(
                    json.dumps(
                        dict(
                            documents=count, elapsed_seconds=time.monotonic() - started
                        )
                    ),
                    flush=True,
                )
        sql.commit()
        writer.commit()
        writer.wait_merging_threads()
        (output / "build.json").write_text(
            json.dumps(
                dict(
                    documents=count,
                    elapsed_seconds=time.monotonic() - started,
                    sqlite_version=sqlite3.sqlite_version,
                    tantivy_version=str(tantivy.__version__),
                ),
                indent=2,
            )
            + "\n"
        )
    index.reload()
    searcher = index.searcher()
    with (output / "runs.tsv").open("w", encoding="utf-8") as stream:
        for line in queries.read_text(encoding="utf-8").splitlines():
            qid, text = line.split("\t", 1)
            expression = " OR ".join(
                '"' + word.replace('"', '""') + '"' for word in text.split()
            )
            rows = [
                item[0]
                for item in sql.execute(
                    "SELECT metadata.url FROM docs JOIN metadata ON metadata.id=docs.rowid WHERE docs MATCH ? ORDER BY bm25(docs), metadata.id LIMIT 5",
                    (expression,),
                )
            ]
            query = index.parse_query(text, ["body"], conjunction_by_default=False)
            hits = searcher.search(query, 5).hits
            alternatives = {
                "sqlite-fts5": rows,
                "tantivy": [searcher.doc(address)["url"][0] for _, address in hits],
            }
            for name, urls in alternatives.items():
                if not urls:
                    stream.write(f"{name}\t{qid}\t0\t-\n")
                for rank, url in enumerate(urls, 1):
                    stream.write(f"{name}\t{qid}\t{rank}\t{url}\n")
    sql.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("corpus", type=Path)
    parser.add_argument("queries", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    run(args.corpus, args.queries, args.output)
