from tests.helpers import connect, rows

import httpx
import pytest

from crawler.config import Config, Source
from crawler.engine import crawl
from crawler.sources import HtmlSource
from tests.helpers import without_robots


@pytest.fixture
def adapter():
    return HtmlSource(
        Source(
            name="test",
            seeds=("https://example.org/index.html",),
            include_paths=("/docs/",),
            exclude_paths=("/docs/private/",),
        )
    )


def test_discover_links(adapter):
    html = """
        <a href="docs/page.html#first">Page</a>
        <a href="/docs/page.html#second">Duplicate</a>
        <a href="/docs/other.html?a=1&amp;b=2">Query</a>
        <a href="/docs/private/page.html">Excluded</a>
        <a href="/docs/book.pdf">PDF</a>
        <a href="https://other.org/docs/page.html">External</a>
        <a href="mailto:test@example.org">Email</a>
        <a href="javascript:void(0)">Script</a>
        <a href="#top">Fragment</a>
        <a href="https://[broken">Invalid</a>
        <a>No href</a>
    """

    assert adapter.discover_links("https://example.org/index.html", html) == (
        "https://example.org/docs/page.html",
        "https://example.org/docs/other.html?a=1&b=2",
    )


def test_base_url(adapter):
    html = '<base href="/docs/"><a href="page.html">Page</a>'

    assert adapter.discover_links("https://example.org/index.html", html) == (
        "https://example.org/docs/page.html",
    )


def test_external_base_does_not_expand_scope(adapter):
    html = '<base href="https://other.org/docs/"><a href="page.html">Page</a>'

    assert adapter.discover_links("https://example.org/index.html", html) == ()


def test_navigation_seed_is_not_document(adapter):
    assert not adapter.is_document("https://example.org/index.html")
    assert adapter.is_document("https://example.org/docs/page.html")


async def test_crawl_discovers_pages_and_skips_navigation(tmp_path, mongo_config):
    config = Config.model_validate(
        {
            "db": mongo_config,
            "logic": {"request_delay_seconds": 0, "timeout_seconds": 5},
            "sources": [
                {
                    "name": "test",
                    "seeds": ["https://example.org/index.html"],
                    "include_paths": ["/docs/"],
                }
            ],
        }
    )
    requests = []
    pages = {
        "/index.html": '<a href="/docs/first.html">First</a><a href="/outside.html">Outside</a>',
        "/docs/first.html": '<a href="second.html">Second</a><a href="/index.html">Index</a>',
        "/docs/second.html": '<a href="first.html#title">Cycle</a>',
    }

    @without_robots
    def respond(request):
        requests.append(request.url.path)
        return httpx.Response(
            200, text=pages[request.url.path], headers={"content-type": "text/html"}
        )

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        assert await crawl(config, client) == 0
        assert await crawl(config, client) == 0

    assert requests == ["/index.html", "/docs/first.html", "/docs/second.html"]

    with connect(config.db) as database:
        assert database.documents.count_documents({}) == 2
        assert rows(database, "jobs", ("status",), sort="_id") == [("done",)] * 3
