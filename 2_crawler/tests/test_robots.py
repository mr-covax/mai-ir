from tests.helpers import connect, rows

import httpx
import pytest

from crawler.config import Config
from crawler.engine import crawl
from crawler.robots import USER_AGENT, load_robots, request_delay


@pytest.fixture
def config(mongo_config):
    return Config.model_validate(
        {
            "db": mongo_config,
            "logic": {"request_delay_seconds": 0, "timeout_seconds": 5},
            "sources": [
                {
                    "name": "test",
                    "seeds": [
                        "https://example.org/public.html",
                        "https://example.org/private.html",
                    ],
                }
            ],
        }
    )


async def test_rules_are_cached_and_blocked_pages_can_be_rechecked(config):
    requests = []
    blocked = True

    def respond(request):
        requests.append(request.url.path)

        if request.url.path == "/robots.txt":
            text = "User-agent: *\nDisallow: /private.html" if blocked else ""
            return httpx.Response(200, text=text)

        return httpx.Response(200, text="<html>OK</html>", headers={"content-type": "text/html"})

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        assert await crawl(config, client) == 0

        with connect(config.db) as database:
            assert rows(database, "jobs", ("status",), sort="_id") == [
                ("done",),
                ("blocked",),
            ]
            assert database.documents.count_documents({}) == 1

        blocked = False
        assert await crawl(config, client) == 0

    assert requests == ["/robots.txt", "/public.html", "/robots.txt", "/private.html"]


@pytest.mark.parametrize("status", [404, 410])
async def test_missing_rules_allow_crawling(status):
    async with httpx.AsyncClient(
        transport=httpx.MockTransport(lambda request: httpx.Response(status))
    ) as client:
        rules = await load_robots(client, "https://example.org/robots.txt")

    assert rules.can_fetch(USER_AGENT, "https://example.org/page")


@pytest.mark.parametrize("status", [401, 403])
async def test_forbidden_rules_block_crawling(config, status):
    requests = []

    def respond(request):
        requests.append(request.url.path)
        return httpx.Response(status)

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        assert await crawl(config, client) == 0

    assert requests == ["/robots.txt"]

    with connect(config.db) as database:
        assert rows(database, "jobs", ("status",), sort="_id") == [("blocked",)] * 2


@pytest.mark.parametrize("status", [302, 429, 500, 503])
async def test_unavailable_rules_fail_closed(config, status):
    requests = []

    def respond(request):
        requests.append(request.url.path)
        return httpx.Response(status)

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        assert await crawl(config, client) == 2

    assert requests == ["/robots.txt"]

    with connect(config.db) as database:
        assert rows(database, "jobs", ("status",), sort="_id") == [("failed",)] * 2
        assert database.documents.count_documents({}) == 0


async def test_rules_timeout_is_not_permission_to_crawl(config):
    requests = []

    def respond(request):
        requests.append(request.url.path)
        raise httpx.ReadTimeout("timeout", request=request)

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        assert await crawl(config, client) == 2

    assert requests == ["/robots.txt"]


async def test_user_agent_and_delay():
    text = """
User-agent: CourseCrawler
Disallow: /private
Crawl-delay: 3
Request-rate: 1/5

User-agent: *
Disallow: /
"""

    async with httpx.AsyncClient(
        transport=httpx.MockTransport(lambda request: httpx.Response(200, text=text))
    ) as client:
        rules = await load_robots(client, "https://example.org/robots.txt")

    assert rules.can_fetch(USER_AGENT, "https://example.org/public")
    assert not rules.can_fetch(USER_AGENT, "https://example.org/private")
    assert request_delay(rules, 2) == 5
    assert request_delay(rules, 10) == 10


async def test_delay_applies_when_first_page_is_blocked(config, monkeypatch):
    source = config.sources[0].model_copy(
        update={"seeds": tuple(reversed(config.sources[0].seeds))}
    )
    config = config.model_copy(update={"sources": (source,)})
    waits = []

    async def wait_for_host(url, next_request, delay):
        waits.append((url, next_request.get("example.org"), delay))
        next_request["example.org"] = 100 + delay

    monkeypatch.setattr("crawler.http.wait_for_host", wait_for_host)

    def respond(request):
        if request.url.path == "/robots.txt":
            return httpx.Response(
                200, text="User-agent: *\nDisallow: /private.html\nCrawl-delay: 3"
            )

        return httpx.Response(200, text="<html>OK</html>", headers={"content-type": "text/html"})

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        assert await crawl(config, client) == 0

    assert waits == [
        ("https://example.org/robots.txt", None, 0),
        ("https://example.org/public.html", 103, 3),
    ]


async def test_http_and_https_have_separate_rules(config):
    source = config.sources[0].model_copy(
        update={
            "seeds": (
                "http://example.org/page.html",
                "https://example.org/page.html",
            )
        }
    )
    config = config.model_copy(update={"sources": (source,)})
    requests = []

    def respond(request):
        requests.append(str(request.url))

        if request.url.path == "/robots.txt":
            return httpx.Response(403 if request.url.scheme == "http" else 404)

        return httpx.Response(200, text="<html>OK</html>", headers={"content-type": "text/html"})

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        assert await crawl(config, client) == 0

    assert requests == [
        "http://example.org/robots.txt",
        "https://example.org/robots.txt",
        "https://example.org/page.html",
    ]
