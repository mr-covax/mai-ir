"""Evaluate the explicitly judged full-corpus pool without opening/building any index.

All source TSV/JSONL files and their assessment provenance live in the lab root.
Only output reports are regenerated. Missing judgments are errors, never grade 0.
"""
import argparse
from collections import Counter
from hashlib import sha256
import json
from pathlib import Path
import subprocess
import tempfile


SOURCES = ("queries.tsv", "intents.tsv", "qrels.tsv", "runs-full.tsv", "assessments-full.jsonl")


def rows(path, count):
    result = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        fields = line.split("\t")
        if len(fields) != count or not all(fields) or "\0" in line:
            raise ValueError(f"{path}:{number}: expected {count} nonempty TSV fields")
        result.append(fields)
    return result


def prepare_inputs(base):
    method = json.loads((base / "assessment-method-full.json").read_text())
    for name in SOURCES:
        digest = sha256((base / name).read_bytes()).hexdigest()
        if digest != method["files_sha256"].get(name):
            raise ValueError(f"Reviewed snapshot changed: {name}; review assessments/provenance")

    queries = [qid for qid, _ in rows(base / "queries.tsv", 2)]
    if not queries or len(set(queries)) != len(queries):
        raise ValueError("Empty or duplicate query IDs")
    intents = [qid for qid, _ in rows(base / "intents.tsv", 2)]
    if len(intents) != len(queries) or set(intents) != set(queries):
        raise ValueError("Query intents do not match query IDs")
    known = set(queries)
    grades = {}
    for qid, url, text in rows(base / "qrels.tsv", 3):
        grade = int(text)
        if qid not in known or grade not in range(4) or (qid, url) in grades:
            raise ValueError("Invalid or duplicate initial judgment")
        grades[qid, url] = grade

    runs = rows(base / "runs-full.tsv", 4)
    required = set()
    system_queries = {}
    for system, qid, rank, url in runs:
        if qid not in known or rank not in {"0", "1", "2", "3", "4", "5"}:
            raise ValueError("Invalid run query ID or rank")
        if (rank == "0") != (url == "-"):
            raise ValueError("Empty result must use rank 0 and URL '-'")
        system_queries.setdefault(system, set()).add(qid)
        if rank != "0":
            required.add((qid, url))
    if not system_queries or any(qids != known for qids in system_queries.values()):
        raise ValueError("Every system must report every query, including empty results")
    if set(system_queries) != set(method["systems"]):
        raise ValueError("Systems differ from the reviewed snapshot")

    reviewed = {}
    for line in (base / "assessments-full.jsonl").read_text().splitlines():
        item = json.loads(line)
        pair = (item["qid"], item["url"])
        grade = item["grade"]
        if type(grade) is not int or grade not in range(4):
            raise ValueError("Assessment grade must be an integer in 0..3")
        if pair in reviewed or pair not in required:
            raise ValueError("Duplicate or out-of-pool assessment")
        if not item["reason"].strip():
            raise ValueError("Assessment requires an explicit rationale")
        fingerprint = item["document_sha256"]
        if len(fingerprint) != 64 or any(c not in "0123456789abcdef" for c in fingerprint):
            raise ValueError("Invalid reviewed document SHA-256")
        reviewed[pair] = grade
    missing = required - reviewed.keys()
    if missing:
        raise ValueError(f"Unjudged pool entries: {len(missing)}; complete explicit assessments")
    if len(reviewed) != method["pool_pairs"]:
        raise ValueError("Assessment count differs from provenance")
    # Explicit new reviews override old snapshot judgments for the same pair only.
    grades.update(reviewed)
    qrels = "".join(f"{qid}\t{url}\t{grade}\n" for (qid, url), grade in sorted(grades.items()))
    coverage = {}
    for system, qids in system_queries.items():
        hits = [(qid, url) for name, qid, rank, url in runs if name == system and rank != "0"]
        coverage[system] = dict(queries=len(qids), hits=len(hits),
                                judged=sum(pair in grades for pair in hits),
                                unjudged=sum(pair not in grades for pair in hits))
    method = dict(method, judged_pairs=len(grades),
                  pool_grade_counts=dict(sorted(Counter(reviewed.values()).items())))
    return qrels, coverage, method


def evaluate(base, output):
    qrels, coverage, method = prepare_inputs(base)
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output) as temporary:
        staging = Path(temporary)
        (staging / "qrels.tsv").write_text(qrels, encoding="utf-8")
        result = subprocess.run(
            [str(base / "build/evaluate"), str(base / "queries.tsv"),
             str(staging / "qrels.tsv"), str(base / "runs-full.tsv")],
            capture_output=True, text=True, timeout=60, check=True,
        )
        json.loads(result.stdout)
        (staging / "metrics.json").write_text(result.stdout, encoding="utf-8")
        (staging / "coverage.json").write_text(json.dumps(coverage, indent=2) + "\n")
        (staging / "assessment-method.json").write_text(json.dumps(method, indent=2) + "\n")
        for path in staging.iterdir():
            path.replace(output / path.name)
    return result.stdout


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    base = Path(__file__).resolve().parents[1]
    parser.add_argument("--output", type=Path, default=base / "data/full")
    args = parser.parse_args()
    try:
        print(evaluate(base, args.output.resolve()), end="")
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        parser.exit(1, f"Evaluation failed: {error}\n")
