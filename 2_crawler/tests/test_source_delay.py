from time import monotonic

import httpx
import pytest
from pydantic import ValidationError

from crawler.config import Config, Source
from crawler.engine import crawl
from tests.helpers import without_robots


@pytest.mark.parametrize("delay", [-1, True, "2", float("nan"), float("inf")])
def test_invalid_source_delay(delay):
    with pytest.raises(ValidationError):
        Source(name="test", seeds=("https://example.org/",), request_delay_seconds=delay)


def test_source_delay_is_optional():
    assert Source(name="test", seeds=("https://example.org/",)).request_delay_seconds is None


@pytest.mark.parametrize("override", [0, 0.5, 5])
async def test_source_delay_overrides_global(mongo_config, monkeypatch, override):
    config = Config.model_validate(
        {
            "db": mongo_config,
            "logic": {"request_delay_seconds": 2, "timeout_seconds": 5},
            "sources": [
                {
                    "name": "custom",
                    "seeds": ["https://first.org/"],
                    "request_delay_seconds": override,
                },
                {"name": "default", "seeds": ["https://second.org/"]},
            ],
        }
    )
    delays = {}

    async def wait(url, next_request, delay):
        delays.setdefault(httpx.URL(url).host, []).append(delay)
        next_request[httpx.URL(url).netloc.decode()] = monotonic() + delay

    monkeypatch.setattr("crawler.http.wait_for_host", wait)
    async with httpx.AsyncClient(
        transport=httpx.MockTransport(
            without_robots(
                lambda request: httpx.Response(
                    200, text="<html>OK</html>", headers={"content-type": "text/html"}
                ),
            )
        )
    ) as client:
        assert await crawl(config, client) == 0

    assert delays == {"first.org": [override, override], "second.org": [2, 2]}


@pytest.mark.parametrize("second_delay,expected", [(None, 2), (0, 0.5), (5, 5)])
async def test_shared_host_uses_strictest_source_delay(
    mongo_config, monkeypatch, second_delay, expected
):
    config = Config.model_validate(
        {
            "db": mongo_config,
            "logic": {"request_delay_seconds": 2, "timeout_seconds": 5},
            "sources": [
                {
                    "name": "first",
                    "seeds": ["https://example.org/first"],
                    "request_delay_seconds": 0.5,
                },
                {
                    "name": "second",
                    "seeds": ["https://example.org/second"],
                    "request_delay_seconds": second_delay,
                },
            ],
        }
    )
    delays = []

    async def wait(url, next_request, delay):
        delays.append(delay)
        next_request[httpx.URL(url).netloc.decode()] = monotonic() + delay

    monkeypatch.setattr("crawler.http.wait_for_host", wait)

    def respond(request):
        if request.url.path == "/robots.txt":
            return httpx.Response(200, text="User-agent: *\nCrawl-delay: 3\n")
        return httpx.Response(200, text="<html>OK</html>", headers={"content-type": "text/html"})

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        assert await crawl(config, client) == 0

    assert delays == [expected, max(expected, 3), max(expected, 3)]
