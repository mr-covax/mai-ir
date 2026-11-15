import json
import subprocess
from pathlib import Path

import pytest


@pytest.fixture
def executable(tmp_path, monkeypatch):
    binary = tmp_path / "tokenizer"
    binary.touch()
    responses = {b"a a a\n": (3, 3), b"bbbbbbbb\n": (1, 8)}

    def respond(arguments, **options):
        data = Path(arguments[-1]).read_bytes()
        tokens, length = responses[data]
        stats = dict(
            tokens=tokens,
            total_length=length,
            average_length=length / tokens,
            input_bytes=len(data),
            elapsed_seconds=0,
            seconds_per_kib=0,
            mib_per_second=0,
        )
        return subprocess.CompletedProcess(arguments, 0, json.dumps(stats), "")

    monkeypatch.setattr("tokenizer.corpus.subprocess.run", respond)
    return binary
