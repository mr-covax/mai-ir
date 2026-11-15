from tests.helpers import connect, rows
from hashlib import sha256

import httpx
import pytest

from crawler.config import Config
from crawler.engine import crawl
from tests.helpers import without_robots


@pytest.fixture
def clock(monkeypatch):
    value = [1000.0]
    monkeypatch.setattr("crawler.storage.time", lambda: value[0])
    return value


@pytest.fixture
def config(mongo_config):
    return Config.model_validate(
        {
            "db": mongo_config,
            "logic": {
                "request_delay_seconds": 0,
                "timeout_seconds": 5,
                "revisit_after_seconds": 10,
            },
            "sources": [{"name": "test", "seeds": ["https://example.org/page.html"]}],
        }
    )


def snapshot(config):
    with connect(config.db) as database:
        document = database.documents.find_one(sort=[("url", 1)])
        job = database.jobs.find_one(sort=[("_id", 1)])

    return dict(document) if document else None, dict(job)


def html_response(html="<html>Original</html>", **headers):
    return httpx.Response(200, text=html, headers={"content-type": "text/html", **headers})


async def test_documents_are_rechecked_only_when_due(config, clock):
    requests = []

    @without_robots
    def respond(request):
        requests.append(request)
        return html_response()

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        assert await crawl(config, client) == 0
        clock[0] = 1009
        assert await crawl(config, client) == 0
        assert len(requests) == 1

        clock[0] = 1010
        assert await crawl(config, client) == 0
        assert len(requests) == 2

        assert await crawl(config, client) == 0
        assert len(requests) == 2

    assert snapshot(config)[1]["checked_at"] == 1010


async def test_304_preserves_document_and_refreshes_validators(config, clock):
    requests = []
    modified = "Wed, 21 Oct 2015 07:28:00 GMT"

    @without_robots
    def respond(request):
        requests.append(request)

        if len(requests) == 1:
            return html_response(etag='"v1"', **{"last-modified": modified})

        return httpx.Response(304, headers={"etag": '"v2"'})

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        assert await crawl(config, client) == 0
        original, _ = snapshot(config)
        clock[0] = 1011
        assert await crawl(config, client) == 0

    document, job = snapshot(config)
    assert requests[1].headers["if-none-match"] == '"v1"'
    assert requests[1].headers["if-modified-since"] == modified
    assert document == original
    assert job["checked_at"] == 1011
    assert job["etag"] == '"v2"'
    assert job["last_modified"] == modified
    assert job["status"] == "done"


async def test_unchanged_200_does_not_rewrite_document(config, clock):
    responses = [html_response(etag='"v1"'), html_response()]

    @without_robots
    def respond(request):
        return responses.pop(0)

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        assert await crawl(config, client) == 0
        original, _ = snapshot(config)
        clock[0] = 1011
        assert await crawl(config, client) == 0

    document, job = snapshot(config)
    assert document == original
    assert job["checked_at"] == 1011
    assert job["etag"] is None
    assert job["last_modified"] is None


async def test_changed_document_is_updated_and_new_links_are_crawled(config, clock):
    requests = []
    html = '<html>Updated<a href="/new.html">New</a></html>'

    @without_robots
    def respond(request):
        requests.append(request.url.path)

        if clock[0] == 1000:
            return html_response(etag='"v1"')

        return html_response(html, etag='"v2"')

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        assert await crawl(config, client) == 0
        clock[0] = 1011
        assert await crawl(config, client) == 0

    assert requests == ["/page.html", "/page.html", "/new.html"]

    with connect(config.db) as database:
        row = rows(
            database,
            "documents",
            ("html", "content_hash", "fetched_at"),
            query={"url": "https://example.org/page.html"},
        )[0]

    assert row == (html, sha256(html.encode()).hexdigest(), 1011)
    assert snapshot(config)[1]["etag"] == '"v2"'


async def test_failed_recheck_preserves_document_and_validators(config, clock):
    requests = []

    @without_robots
    def respond(request):
        requests.append(request)

        if len(requests) == 1:
            return html_response(etag='"v1"')

        if len(requests) == 2:
            return httpx.Response(503)

        return httpx.Response(304)

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        assert await crawl(config, client) == 0
        original, _ = snapshot(config)
        clock[0] = 1011
        assert await crawl(config, client) == 1
        document, job = snapshot(config)
        assert document == original
        assert job["checked_at"] == 1000
        assert job["etag"] == '"v1"'
        assert job["status"] == "failed"

        assert await crawl(config, client) == 0

    assert requests[2].headers["if-none-match"] == '"v1"'
    assert snapshot(config)[0] == original
    assert snapshot(config)[1]["last_error"] is None


async def test_unsolicited_304_is_rejected(config, clock):
    @without_robots
    def respond(request):
        return httpx.Response(304)

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        assert await crawl(config, client) == 1

    document, job = snapshot(config)
    assert document is None
    assert job["status"] == "failed"
    assert job["checked_at"] is None


async def test_navigation_pages_are_rechecked(config, clock):
    source = config.sources[0].model_copy(update={"include_paths": ("/articles/",)})
    config = config.model_copy(update={"sources": (source,)})
    requests = []

    @without_robots
    def respond(request):
        requests.append(request)

        if len(requests) == 1:
            return html_response(etag='"index-v1"')

        return httpx.Response(304)

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        assert await crawl(config, client) == 0
        clock[0] = 1011
        assert await crawl(config, client) == 0
        assert await crawl(config, client) == 0

    assert len(requests) == 2
    assert requests[1].headers["if-none-match"] == '"index-v1"'
    document, job = snapshot(config)
    assert document is None
    assert job["checked_at"] == 1011
    assert job["status"] == "done"


async def test_last_modified_without_etag(config, clock):
    requests = []
    modified = "Wed, 21 Oct 2015 07:28:00 GMT"

    @without_robots
    def respond(request):
        requests.append(request)

        if len(requests) == 1:
            return html_response(**{"last-modified": modified})

        return httpx.Response(304)

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        assert await crawl(config, client) == 0
        clock[0] = 1011
        assert await crawl(config, client) == 0

    assert "if-none-match" not in requests[1].headers
    assert requests[1].headers["if-modified-since"] == modified
    assert snapshot(config)[1]["last_modified"] == modified


async def test_zero_interval_rechecks_once_per_run(config, clock):
    logic = config.logic.model_copy(update={"revisit_after_seconds": 0})
    config = config.model_copy(update={"logic": logic})
    requests = []

    @without_robots
    def respond(request):
        requests.append(request)
        return html_response()

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        assert await crawl(config, client) == 0
        assert len(requests) == 1
        assert await crawl(config, client) == 0
        assert len(requests) == 2


@pytest.mark.parametrize("interval", [-1, True, "10", float("nan"), float("inf")])
def test_invalid_revisit_interval(config, interval):
    data = config.model_dump()
    data["logic"]["revisit_after_seconds"] = interval

    with pytest.raises(ValueError):
        Config.model_validate(data)
