import argparse
import json
import re
import sqlite3
from pathlib import Path


def collect(corpus, metadata, queries, runs, output, start, end):
    database = sqlite3.connect(metadata)
    questions = dict(line.split("\t", 1) for line in queries.read_text().splitlines())
    old = json.loads(output.read_text()) if output.exists() else []
    known = {(row["qid"], row["url"]) for row in old}
    with (corpus / "corpus.bin").open("rb") as stream:
        for file in runs:
            for line in file.read_text().splitlines():
                system, qid, rank, url = line.split("\t")
                if rank == "0" or (qid, url) in known:
                    continue
                known.add((qid, url))
                row = database.execute(
                    "SELECT title,offset,bytes FROM metadata WHERE url=?", (url,)
                ).fetchone()
                if not row:
                    raise ValueError(f"Missing document: {url}")
                title, offset, size = row
                stream.seek(offset)
                text = stream.read(size).decode()
                words = re.findall(r"[\w+:]+", questions[qid].lower())
                excerpts = [text[:260]]
                for word in sorted(words, key=len, reverse=True)[:2]:
                    at = text.lower().find(word)
                    if at >= 0:
                        excerpts.append(text[max(0, at - 100) : at + 260])
                old.append(
                    dict(
                        code=f"p{len(old) + 1:04}",
                        qid=qid,
                        url=url,
                        title=title,
                        snippet=" | ".join(" ".join(part.split()) for part in excerpts),
                    )
                )
    output.write_text(json.dumps(old, ensure_ascii=False, indent=2) + "\n")
    for row in old:
        if start <= int(row["qid"][1:]) <= end:
            print(
                row["code"],
                row["qid"],
                row["url"],
                row["title"][:190],
                row["snippet"][:650],
                sep="\t",
            )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("corpus", type=Path)
    parser.add_argument("metadata", type=Path)
    parser.add_argument("queries", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("runs", nargs="+", type=Path)
    parser.add_argument("--start", type=int, default=1)
    parser.add_argument("--end", type=int, default=30)
    args = parser.parse_args()
    collect(
        args.corpus,
        args.metadata,
        args.queries,
        args.runs,
        args.output,
        args.start,
        args.end,
    )
