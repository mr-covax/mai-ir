import hashlib
import json
from pathlib import Path
import runpy
import tempfile
import unittest

MODULE = runpy.run_path(str(Path(__file__).resolve().parents[1] / "tools/evaluate_full.py"))


class FullEvaluationInputs(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        (self.base / "queries.tsv").write_text("q1\ttest\nq2\tempty\n")
        (self.base / "intents.tsv").write_text("q1\tFind test\nq2\tNo hits expected\n")
        (self.base / "qrels.tsv").write_text("q1\thttps://example.test/old\t2\n")
        (self.base / "runs-full.tsv").write_text(
            "system\tq1\t1\thttps://example.test/new\nsystem\tq2\t0\t-\n")
        self.review = dict(qid="q1", url="https://example.test/new", grade=3,
                           reason="Explicitly reviewed direct answer", document_sha256="a" * 64)
        self.set_reviews([self.review])

    def set_reviews(self, reviews):
        (self.base / "assessments-full.jsonl").write_text(
            "".join(json.dumps(row) + "\n" for row in reviews))
        self.fingerprint()

    def fingerprint(self):
        method = dict(files_sha256={name: hashlib.sha256((self.base / name).read_bytes()).hexdigest()
                                    for name in MODULE["SOURCES"]},
                      systems={"system": "test"}, pool_pairs=1)
        (self.base / "assessment-method-full.json").write_text(json.dumps(method))

    def prepare(self):
        return MODULE["prepare_inputs"](self.base)

    def test_complete_pool_preserves_old_judgments_and_empty_queries(self):
        qrels, coverage, method = self.prepare()
        self.assertIn("q1\thttps://example.test/old\t2", qrels)
        self.assertIn("q1\thttps://example.test/new\t3", qrels)
        self.assertEqual(coverage["system"], dict(queries=2, hits=1, judged=1, unjudged=0))
        self.assertEqual(method["judged_pairs"], 2)

    def test_missing_assessment_is_not_implicitly_zero(self):
        self.set_reviews([])
        with self.assertRaisesRegex(ValueError, "Unjudged"):
            self.prepare()

    def test_duplicate_assessment_rejected(self):
        self.set_reviews([self.review, self.review])
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            self.prepare()

    def test_changed_snapshot_rejected(self):
        (self.base / "queries.tsv").write_text("q1\tchanged intent\n")
        with self.assertRaisesRegex(ValueError, "snapshot changed"):
            self.prepare()

    def test_invalid_grades_rejected(self):
        for grade in (-1, 4, True, 2.5, "3"):
            with self.subTest(grade=grade):
                self.set_reviews([dict(self.review, grade=grade)])
                with self.assertRaisesRegex(ValueError, "grade"):
                    self.prepare()

    def test_missing_system_query_rejected(self):
        (self.base / "runs-full.tsv").write_text("system\tq1\t1\thttps://example.test/new\n")
        self.fingerprint()
        with self.assertRaisesRegex(ValueError, "every query"):
            self.prepare()

    def test_review_requires_rationale(self):
        self.set_reviews([dict(self.review, reason=" ")])
        with self.assertRaisesRegex(ValueError, "rationale"):
            self.prepare()


if __name__ == "__main__":
    unittest.main()
