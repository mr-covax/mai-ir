from copy import deepcopy

import pytest
from pydantic import ValidationError

from crawler.config import Config


@pytest.fixture
def data():
    return {
        "db": {"uri": "mongodb://localhost:27017/", "name": "crawler"},
        "logic": {"request_delay_seconds": 0, "timeout_seconds": 5},
        "sources": [{"name": "test", "seeds": ["https://example.org/"]}],
    }


@pytest.mark.parametrize("section", [None, "db", "logic", "source"])
def test_unknown_fields(data, section):
    target = (
        data if section is None else data["sources"][0] if section == "source" else data[section]
    )
    target["typo"] = True

    with pytest.raises(ValidationError, match="extra_forbidden"):
        Config.model_validate(data)


def test_duplicate_source_names(data):
    data["sources"].append(deepcopy(data["sources"][0]))

    with pytest.raises(ValidationError, match="Source names must be unique"):
        Config.model_validate(data)


def test_duplicate_seeds(data):
    data["sources"][0]["seeds"] = ["https://example.org/#a", "https://example.org/#b"]

    assert Config.model_validate(data).sources[0].seeds == ("https://example.org/",)


@pytest.mark.parametrize("uri", ["", "   ", None, 123, "sqlite:///data", "mongodb://"])
def test_invalid_database_uri(data, uri):
    data["db"]["uri"] = uri

    with pytest.raises(ValidationError):
        Config.model_validate(data)


@pytest.mark.parametrize("timeout", [0, -1, True, "5", float("inf"), float("nan")])
def test_invalid_timeout(data, timeout):
    data["logic"]["timeout_seconds"] = timeout

    with pytest.raises(ValidationError):
        Config.model_validate(data)
