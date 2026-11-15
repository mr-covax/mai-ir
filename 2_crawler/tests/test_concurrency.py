import asyncio
from pymongo.errors import OperationFailure
from tests.helpers import connect, rows
from time import monotonic

import httpx
import pytest

from crawler.config import Config
from crawler.engine import crawl
from tests.helpers import without_robots


def make_config(mongo_config, sources, delay=0):
    return Config.model_validate(
        {
            "db": mongo_config,
            "logic": {"request_delay_seconds": delay, "timeout_seconds": 5},
            "sources": sources,
        }
    )


def html_response():
    return httpx.Response(200, text="<html>OK</html>", headers={"content-type": "text/html"})


async def test_hosts_run_concurrently(mongo_config):
    config = make_config(
        mongo_config,
        [
            {
                "name": "test",
                "seeds": [
                    "https://first.org/page",
                    "https://second.org/page",
                ],
            }
        ],
    )
    started = {host: asyncio.Event() for host in ("first.org", "second.org")}

    @without_robots
    async def respond(request):
        started[request.url.host].set()
        other = "second.org" if request.url.host == "first.org" else "first.org"
        await started[other].wait()
        return html_response()

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        async with asyncio.timeout(2):
            assert await crawl(config, client) == 0

    with connect(config.db) as database:
        assert rows(database, "jobs", ("status", "attempts"), sort="_id") == [
            ("done", 1),
            ("done", 1),
        ]
        assert database.documents.count_documents({}) == 2


async def test_sources_on_same_host_share_worker_and_delay(mongo_config):
    config = make_config(
        mongo_config,
        [
            {"name": "first", "seeds": ["https://example.org/first"]},
            {"name": "second", "seeds": ["https://example.org/second"]},
        ],
        delay=0.05,
    )
    active = 0
    starts = []

    @without_robots
    async def respond(request):
        nonlocal active
        active += 1
        assert active == 1
        starts.append(monotonic())
        await asyncio.sleep(0.01)
        active -= 1
        return html_response()

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        assert await crawl(config, client) == 0

    assert len(starts) == 2
    assert starts[1] - starts[0] >= 0.045


async def test_waiting_host_does_not_block_other_hosts(mongo_config, monkeypatch):
    config = make_config(
        mongo_config,
        [
            {
                "name": "test",
                "seeds": [
                    "https://first.org/page",
                    "https://second.org/page",
                ],
            }
        ],
    )
    waiting = asyncio.Event()
    release = asyncio.Event()
    completed = []

    async def wait_for_host(url, next_request, delay):
        next_request[httpx.URL(url).netloc.decode()] = monotonic()

        if "first.org" in url:
            waiting.set()
            await release.wait()

    monkeypatch.setattr("crawler.http.wait_for_host", wait_for_host)

    @without_robots
    async def respond(request):
        if request.url.host == "second.org":
            await waiting.wait()
            completed.append("second")
            release.set()
        else:
            completed.append("first")

        return html_response()

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        async with asyncio.timeout(2):
            assert await crawl(config, client) == 0

    assert completed == ["second", "first"]


async def test_database_error_stops_other_workers(mongo_config, monkeypatch):
    config = make_config(
        mongo_config,
        [
            {
                "name": "test",
                "seeds": [
                    "https://first.org/page",
                    "https://second.org/page",
                ],
            }
        ],
    )
    second_started = asyncio.Event()
    second_stopped = asyncio.Event()

    @without_robots
    async def respond(request):
        if request.url.host == "first.org":
            await second_started.wait()
            return html_response()

        second_started.set()

        try:
            await asyncio.Event().wait()
        finally:
            second_stopped.set()

    async def complete_job(database, result):
        raise OperationFailure("Database unavailable")

    monkeypatch.setattr("crawler.engine.complete_job", complete_job)

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        async with asyncio.timeout(2):
            with pytest.raises(OperationFailure, match="Database unavailable"):
                await crawl(config, client)

    assert second_stopped.is_set()


async def test_cancellation_stops_all_workers_and_allows_resume(mongo_config):
    config = make_config(
        mongo_config,
        [
            {
                "name": "test",
                "seeds": [
                    "https://first.org/page",
                    "https://second.org/page",
                ],
            }
        ],
    )
    started = []
    stopped = []
    ready = asyncio.Event()

    @without_robots
    async def respond(request):
        started.append(request.url.host)

        if len(started) == 2:
            ready.set()

        try:
            await asyncio.Event().wait()
        finally:
            stopped.append(request.url.host)

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        task = asyncio.create_task(crawl(config, client))

        try:
            async with asyncio.timeout(2):
                await ready.wait()
        finally:
            task.cancel()

            with pytest.raises(asyncio.CancelledError):
                await task

    assert sorted(stopped) == ["first.org", "second.org"]

    async with httpx.AsyncClient(
        transport=httpx.MockTransport(without_robots(lambda request: html_response()))
    ) as client:
        assert await crawl(config, client) == 0

    with connect(config.db) as database:
        assert rows(database, "jobs", ("status", "attempts"), sort="_id") == [
            ("done", 2),
            ("done", 2),
        ]
