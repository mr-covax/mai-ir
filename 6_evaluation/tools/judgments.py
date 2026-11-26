import argparse
import json
from hashlib import file_digest
from pathlib import Path
from urllib.parse import urlsplit


def expand(corpus, decisions, queries, output):
    with (corpus / "corpus.bin").open("rb") as stream:
        fingerprint = file_digest(stream, "sha256").hexdigest()
    if fingerprint != decisions.with_suffix(".sha256").read_text().split()[0]:
        raise ValueError(
            "Corpus differs from reviewed snapshot; revise judgments before evaluation"
        )
    documents = [
        json.loads(line)
        for line in (corpus / "manifest.jsonl").read_text(encoding="utf-8").splitlines()
    ]
    names = {Path(urlsplit(document["url"]).path).name for document in documents}
    if len(names) != len(documents):
        raise ValueError("Document basenames are ambiguous")
    grades = json.loads(decisions.read_text(encoding="utf-8"))
    identifiers = [
        line.split("\t")[0] for line in queries.read_text(encoding="utf-8").splitlines()
    ]
    if set(identifiers) != grades.keys():
        raise ValueError("Query set differs from editorial judgments")
    with output.open("w", encoding="utf-8") as stream:
        for qid in identifiers:
            assigned = {}
            for grade, items in grades[qid].items():
                if int(grade) not in (1, 2, 3):
                    raise ValueError("Grades must be 1, 2 or 3")
                for name in items:
                    if name not in names or name in assigned:
                        raise ValueError(f"Unknown or duplicate judgment: {qid} {name}")
                    assigned[name] = int(grade)
            for document in documents:
                name = Path(urlsplit(document["url"]).path).name
                stream.write(f"{qid}\t{document['url']}\t{assigned.get(name, 0)}\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("corpus", type=Path)
    parser.add_argument("decisions", type=Path)
    parser.add_argument("queries", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    expand(args.corpus, args.decisions, args.queries, args.output)
