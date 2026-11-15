import argparse
import logging
from pymongo.errors import PyMongoError
import sys
from pathlib import Path

from tokenizer.corpus import tokenize_corpus
from tokenizer.export import export_corpus
from tokenizer.extraction import extract_document
from tokenizer.profiles import SOURCES


def main() -> None:
    parser = argparse.ArgumentParser(description="Extract text from programming documentation HTML")
    parser.add_argument("input", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--source", choices=sorted(SOURCES), default="linux-kernel")
    arguments = parser.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")

    try:
        html = arguments.input.read_text(encoding="utf-8")
        document = extract_document(html, arguments.source)
        text = document.text + "\n"

        if arguments.output is None:
            sys.stdout.write(text)
        else:
            arguments.output.write_text(text, encoding="utf-8")

    except (OSError, ValueError) as error:
        logging.error("%s", error)
        raise SystemExit(1) from None


def export_main() -> None:
    parser = argparse.ArgumentParser(
        description="Export crawler documents as UTF-8 text and a JSONL manifest"
    )
    parser.add_argument(
        "database", help="MongoDB URI including database name (replica set required)"
    )
    parser.add_argument("output", type=Path)
    parser.add_argument("--source", choices=sorted(SOURCES), action="append", default=[])
    arguments = parser.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")

    try:
        stats = export_corpus(arguments.database, arguments.output, tuple(arguments.source))
        logging.info("Exported %d documents, %d text bytes", stats.documents, stats.text_bytes)
    except (OSError, ValueError, PyMongoError) as error:
        logging.error("%s", error)
        raise SystemExit(1) from None
    except KeyboardInterrupt:
        raise SystemExit(130) from None


def corpus_main() -> None:
    parser = argparse.ArgumentParser(
        description="Tokenize an exported corpus and aggregate statistics"
    )
    parser.add_argument("corpus", type=Path)
    parser.add_argument("--binary", type=Path, default=Path("build/tokenize"))
    parser.add_argument("--limit", type=int)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=60)
    arguments = parser.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")

    try:
        report = tokenize_corpus(
            arguments.corpus,
            arguments.binary,
            limit=arguments.limit,
            repeat=arguments.repeat,
            timeout=arguments.timeout,
        )
        sys.stdout.write(report.model_dump_json(indent=2) + "\n")
    except (OSError, ValueError) as error:
        logging.error("%s", error)
        raise SystemExit(1) from None
    except KeyboardInterrupt:
        raise SystemExit(130) from None
