import argparse
import json
import sqlite3
from pathlib import Path


def finalize(root, initial):
    pool = json.loads((root / "pool.json").read_text())
    judgments = dict(
        line.split("\t") for line in (root / "assessments.tsv").read_text().splitlines()
    )
    missing = [row["code"] for row in pool if row["code"] not in judgments]
    if missing:
        raise ValueError(f"Unjudged pool entries: {missing}")
    database = sqlite3.connect(root / "sqlite.db")
    grades = {}
    for line in initial.read_text().splitlines():
        qid, url, grade = line.split("\t")
        if database.execute("SELECT 1 FROM metadata WHERE url=?", (url,)).fetchone():
            grades[(qid, url)] = int(grade)
    for row in pool:
        grade = int(judgments[row["code"]])
        if grade not in range(4):
            raise ValueError("Grade outside 0..3")
        grades[(row["qid"], row["url"])] = grade
    with (root / "qrels.tsv").open("w") as output:
        for (qid, url), grade in sorted(grades.items()):
            output.write(f"{qid}\t{url}\t{grade}\n")
    (root / "assessment-method.json").write_text(
        json.dumps(
            dict(
                assessor="assistant",
                independent_human_assessors=0,
                scale=dict(primary=3, useful=2, background=1, irrelevant=0),
                relevant_minimum=2,
                method="Explicit editorial assessment of pooled top-5 results using query intents, titles and source text excerpts; preserved known reference documents",
                exhaustive_corpus_judgments=False,
                pool_entries=len(pool),
                judged_pairs=len(grades),
            ),
            ensure_ascii=False,
            indent=2,
        )
        + "\n"
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("results", type=Path)
    parser.add_argument("initial", type=Path)
    args = parser.parse_args()
    finalize(args.results, args.initial)
