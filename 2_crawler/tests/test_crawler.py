from tests.helpers import connect, rows

import httpx
import pytest
import yaml

from crawler.config import Config, Source, load_config, normalize_url
from crawler.engine import crawl
from tests.helpers import without_robots


@pytest.fixture
def config_file(tmp_path, mongo_config):
    path = tmp_path / "config.yaml"
    data = {
        "db": mongo_config,
        "logic": {"request_delay_seconds": 0, "timeout_seconds": 5},
        "sources": [{"name": "test", "seeds": ["https://example.org/page#title"]}],
    }
    path.write_text(yaml.safe_dump(data), encoding="utf-8")
    return path


def test_config(config_file, mongo_config):
    config = load_config(config_file)

    assert config.db.model_dump() == mongo_config
    assert config.sources == (Source(name="test", seeds=("https://example.org/page",)),)
    assert config.logic.timeout_seconds == 5


@pytest.mark.parametrize("content", ["", "[]", "db: [", "db: {}", "sources: []"])
def test_invalid_config(tmp_path, content):
    path = tmp_path / "config.yaml"
    path.write_text(content, encoding="utf-8")

    with pytest.raises(ValueError):
        load_config(path)


@pytest.mark.parametrize("value", [-1, True, float("nan"), float("inf"), "2"])
def test_invalid_delay(config_file, value):
    data = yaml.safe_load(config_file.read_text())
    data["logic"]["request_delay_seconds"] = value
    config_file.write_text(yaml.safe_dump(data))

    with pytest.raises(ValueError):
        load_config(config_file)


def test_normalize_url():
    assert (
        normalize_url("HTTPS://Example.org:443/path?q=1#section") == "https://example.org/path?q=1"
    )
    assert normalize_url("http://example.org") == "http://example.org/"


@pytest.mark.parametrize("url", ["file:///tmp/page", "https://", "https://user:pass@example.org/"])
def test_invalid_url(url):
    with pytest.raises(ValueError):
        normalize_url(url)


async def test_save_and_skip_completed(config_file):
    config = load_config(config_file)
    html = "<html><body>Первый документ</body></html>"
    requests = []

    @without_robots
    def respond(request):
        requests.append(request)
        return httpx.Response(200, text=html, headers={"content-type": "text/html; charset=utf-8"})

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        assert await crawl(config, client) == 0
        assert await crawl(config, client) == 0

    assert len(requests) == 1

    with connect(config.db) as database:
        documents = rows(database, "documents", ("url", "html", "source", "fetched_at"), sort="_id")

    assert len(documents) == 1
    assert documents[0][:3] == ("https://example.org/page", html, "test")
    assert isinstance(documents[0][3], int)
    assert documents[0][3] > 0


@pytest.mark.parametrize(
    "status, content_type",
    [(404, "text/html"), (500, "text/html"), (200, "application/pdf"), (302, "text/html")],
)
async def test_failed_page_not_saved(config_file, status, content_type):
    config = load_config(config_file)

    @without_robots
    def respond(request):
        return httpx.Response(status, headers={"content-type": content_type})

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        assert await crawl(config, client) == 1

    with connect(config.db) as database:
        assert database.documents.count_documents({}) == 0


async def test_timeout_continues_with_next_page(tmp_path, mongo_config):
    config = Config.model_validate(
        {
            "db": mongo_config,
            "logic": {"request_delay_seconds": 0, "timeout_seconds": 5},
            "sources": [
                {
                    "name": "test",
                    "seeds": [
                        "https://example.org/bad",
                        "https://example.org/good",
                    ],
                }
            ],
        }
    )

    @without_robots
    def respond(request):
        if request.url.path == "/bad":
            raise httpx.ReadTimeout("timeout", request=request)

        return httpx.Response(200, text="<html>OK</html>", headers={"content-type": "text/html"})

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        assert await crawl(config, client) == 1

    with connect(config.db) as database:
        assert rows(database, "documents", ("url",), sort="_id") == [("https://example.org/good",)]
