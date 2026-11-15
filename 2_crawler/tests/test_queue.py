import asyncio
from pymongo.errors import OperationFailure
from tests.helpers import connect, rows

import httpx
import pytest

from crawler.config import Config, Source
from crawler.engine import crawl
from crawler.models import CrawlResult, Page
from tests.helpers import without_robots
from crawler.storage import claim_job, complete_job, open_database, prepare_queue, save_document


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
                        "https://example.org/first",
                        "https://example.org/second",
                    ],
                }
            ],
        }
    )


def html_response():
    return httpx.Response(200, text="<html>OK</html>", headers={"content-type": "text/html"})


async def test_resume_after_cancellation(config):
    requests = []

    @without_robots
    async def interrupt(request):
        requests.append(request.url.path)

        if request.url.path == "/second":
            raise asyncio.CancelledError

        return html_response()

    async with httpx.AsyncClient(transport=httpx.MockTransport(interrupt)) as client:
        with pytest.raises(asyncio.CancelledError):
            await crawl(config, client)

    with connect(config.db) as database:
        assert rows(database, "jobs", ("status",), sort="_id") == [
            ("done",),
            ("processing",),
        ]

    @without_robots
    def respond(request):
        requests.append(request.url.path)
        return html_response()

    async with httpx.AsyncClient(transport=httpx.MockTransport(respond)) as client:
        assert await crawl(config, client) == 0

    assert requests == ["/first", "/second", "/second"]

    with connect(config.db) as database:
        assert rows(database, "jobs", ("status", "attempts"), sort="_id") == [
            ("done", 1),
            ("done", 2),
        ]
        assert database.documents.count_documents({}) == 2


async def test_failed_job_retried_on_next_run(config):
    @without_robots
    def fail(request):
        return httpx.Response(503)

    async with httpx.AsyncClient(transport=httpx.MockTransport(fail)) as client:
        assert await crawl(config, client) == 2

    with connect(config.db) as database:
        jobs = rows(database, "jobs", ("status", "attempts", "last_error"), sort="_id")
        assert all(
            status == "failed" and attempts == 1 and error for status, attempts, error in jobs
        )

    async with httpx.AsyncClient(
        transport=httpx.MockTransport(without_robots(lambda request: html_response()))
    ) as client:
        assert await crawl(config, client) == 0

    with connect(config.db) as database:
        assert rows(database, "jobs", ("status", "attempts", "last_error"), sort="_id") == [
            ("done", 2, None),
            ("done", 2, None),
        ]


async def test_queue_is_persistent_and_scoped(config):
    async with open_database(config.db) as database:
        await prepare_queue(database, config.sources)
        await prepare_queue(database, config.sources)
        await prepare_queue(database, (Source(name="other", seeds=("https://other.org/",)),))

    async with open_database(config.db) as database:
        first = await claim_job(database, ("test",))
        second = await claim_job(database, ("test",))

        assert first.url == "https://example.org/first"
        assert second.url == "https://example.org/second"
        assert await claim_job(database, ("test",)) is None
        assert (await claim_job(database, ("other",))).url == "https://other.org/"


async def test_existing_documents_are_skipped(config):
    async with open_database(config.db) as database:
        await save_document(database, config.sources[0].seeds[0], "old", "test")
        await prepare_queue(database, config.sources)

        assert (await claim_job(database, ("test",))).url.endswith("/second")
        assert await claim_job(database, ("test",)) is None


async def test_completion_is_atomic(config):
    async with open_database(config.db) as database:
        await prepare_queue(database, config.sources)
        job = await claim_job(database, ("test",))
        await database.command(
            {
                "collMod": "documents",
                "validator": {"html": {"$ne": "html"}},
                "validationLevel": "strict",
            }
        )

        with pytest.raises(OperationFailure):
            await complete_job(
                database,
                CrawlResult(
                    job=job,
                    page=Page(html="html"),
                    links=("https://example.org/discovered",),
                ),
            )

        assert (await database.jobs.find_one({"url": job.url}))["status"] == "processing"
        assert await database.jobs.count_documents({"url": "https://example.org/discovered"}) == 0


async def test_document_upsert(config):
    async with open_database(config.db) as database:
        url = config.sources[0].seeds[0]
        await save_document(database, url, "old", "test")
        await save_document(database, url, "new", "test")

        assert [row["html"] async for row in database.documents.find()] == ["new"]
