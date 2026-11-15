from tests.helpers import connect, rows

import httpx
import pytest

from crawler.config import Config
from crawler.engine import crawl
from crawler.http import resolve_redirect
from crawler.models import Redirect
from tests.helpers import without_robots


@pytest.fixture
def config(mongo_config):
    return Config.model_validate(
        {
            "db": mongo_config,
            "logic": {"request_delay_seconds": 0, "timeout_seconds": 5},
            "sources": [
                {
                    "name": "test",
                    "seeds": ["https://example.org/docs/old.html"],
                    "include_paths": ["/docs/"],
                }
            ],
        }
    )


def test_relative_redirect():
    assert resolve_redirect("https://example.org/docs/old.html", "new/page.html#top") == Redirect(
        url="https://example.org/docs/new/page.html",
    )


def test_https_is_preserved():
    assert resolve_redirect(
        "https://example.org/docs/old.html", "http://example.org/docs/new.html"
    ).url == ("https://example.org/docs/new.html")


@pytest.mark.parametrize(
    "location",
    [
        None,
        "",
        "#top",
        "https://other.org/docs/page.html",
        "file:///tmp/page",
        "https://user:pass@example.org/docs/new.html",
    ],
)
def test_unsafe_redirect(location):
    with pytest.raises(ValueError):
        resolve_redirect("https://example.org/docs/old.html", location)


@pytest.mark.parametrize("status", [301, 302, 303, 307, 308])
async def test_redirect_target_is_queued_and_relative_links_use_target_url(config, status):
    requests = []

    @without_robots
    def respond(request):
        requests.append(request.url.path)

        if request.url.path == "/docs/old.html":
            return httpx.Response(status, headers={"location": "new/page.html"})

        html = (
            '<a href="child.html">Child</a>'
            if request.url.path.endswith("page.html")
            else "<html>Child</html>"
        )
        return httpx.Response(200, text=html, headers={"content-type": "text/html"})

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        assert await crawl(config, client) == 0
        assert await crawl(config, client) == 0

    assert requests == ["/docs/old.html", "/docs/new/page.html", "/docs/new/child.html"]

    with connect(config.db) as database:
        assert rows(database, "documents", ("url",), sort="url") == [
            ("https://example.org/docs/new/child.html",),
            ("https://example.org/docs/new/page.html",),
        ]
        assert rows(database, "jobs", ("status",), sort="_id") == [("done",)] * 3


@pytest.mark.parametrize(
    "target", ["https://other.org/docs/new.html", "/outside/page.html", "/docs/book.pdf"]
)
async def test_redirect_cannot_escape_source(config, target):
    requests = []

    @without_robots
    def respond(request):
        requests.append(request.url.path)
        return httpx.Response(302, headers={"location": target})

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        assert await crawl(config, client) == 1

    assert requests == ["/docs/old.html"]

    with connect(config.db) as database:
        assert rows(database, "jobs", ("status",), sort="_id") == [("failed",)]


async def test_redirect_target_is_checked_against_robots(config):
    requests = []

    def respond(request):
        requests.append(request.url.path)

        if request.url.path == "/robots.txt":
            return httpx.Response(200, text="User-agent: *\nDisallow: /docs/private.html")

        return httpx.Response(302, headers={"location": "/docs/private.html"})

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        assert await crawl(config, client) == 0

    assert requests == ["/robots.txt", "/docs/old.html"]

    with connect(config.db) as database:
        assert rows(database, "jobs", ("status",), sort="_id") == [("done",), ("blocked",)]


async def test_redirect_cycle_does_not_repeat_jobs(config):
    requests = []

    @without_robots
    def respond(request):
        requests.append(request.url.path)
        target = "/docs/new.html" if request.url.path.endswith("old.html") else "/docs/old.html"
        return httpx.Response(302, headers={"location": target})

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        assert await crawl(config, client) == 0

    assert requests == ["/docs/old.html", "/docs/new.html"]


async def test_old_validators_are_not_sent_to_redirect_target(config):
    responses = 0

    @without_robots
    def respond(request):
        nonlocal responses
        responses += 1

        if responses == 1:
            return httpx.Response(
                200, text="Old", headers={"content-type": "text/html", "etag": '"old"'}
            )

        if request.url.path.endswith("old.html"):
            assert request.headers["if-none-match"] == '"old"'
            return httpx.Response(301, headers={"location": "/docs/new.html"})

        assert "if-none-match" not in request.headers
        return httpx.Response(
            200, text="New", headers={"content-type": "text/html", "etag": '"new"'}
        )

    config = config.model_copy(
        update={"logic": config.logic.model_copy(update={"revisit_after_seconds": 0})}
    )

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        assert await crawl(config, client) == 0
        assert await crawl(config, client) == 0

    assert responses == 3
