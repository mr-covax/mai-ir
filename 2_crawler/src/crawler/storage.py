from contextlib import asynccontextmanager
from hashlib import sha256
from time import time
from urllib.parse import urlsplit

from pymongo import AsyncMongoClient, ReturnDocument
from pymongo.asynchronous.database import AsyncDatabase

from crawler.config import DatabaseConfig, Source
from crawler.models import CrawlResult, Job


@asynccontextmanager
async def open_database(config: DatabaseConfig):
    async with AsyncMongoClient(config.uri, serverSelectionTimeoutMS=5000) as client:
        database = client[config.name]
        await database.documents.create_index("url", unique=True)
        await database.jobs.create_index("url", unique=True)
        await database.jobs.create_index([("status", 1), ("source", 1), ("host", 1), ("_id", 1)])
        yield database


async def prepare_queue(
    database: AsyncDatabase,
    sources: tuple[Source, ...],
    revisit_after: float = 86400,
) -> None:
    cutoff = time() - revisit_after
    for source in sources:
        await database.jobs.update_many(
            {"source": source.name, "status": {"$in": ["processing", "failed", "blocked"]}},
            {"$set": {"status": "pending"}},
        )
        await enqueue_urls(database, source.name, source.seeds)
        await database.jobs.update_many(
            {
                "source": source.name,
                "status": "done",
                "$or": [
                    {"checked_at": None},
                    {"checked_at": {"$lte": cutoff}},
                ],
            },
            {"$set": {"status": "pending"}},
        )


async def enqueue_urls(
    database: AsyncDatabase, source: str, urls: tuple[str, ...], *, session=None
) -> None:
    for url in urls:
        document = await database.documents.find_one({"url": url}, session=session)
        await database.jobs.update_one(
            {"url": url},
            {
                "$setOnInsert": {
                    "url": url,
                    "host": urlsplit(url).netloc,
                    "source": source,
                    "status": "done" if document else "pending",
                    "checked_at": document["fetched_at"] if document else None,
                    "attempts": 0,
                    "last_error": None,
                    "etag": None,
                    "last_modified": None,
                }
            },
            upsert=True,
            session=session,
        )


async def save_document(
    database: AsyncDatabase, url: str, html: str, source: str, *, session=None
) -> None:
    content_hash = sha256(html.encode("utf-8")).hexdigest()
    existing = await database.documents.find_one({"url": url}, session=session)
    if existing and existing["content_hash"] == content_hash:
        return
    await database.documents.update_one(
        {"url": url},
        {
            "$set": {
                "html": html,
                "source": source,
                "fetched_at": int(time()),
                "content_hash": content_hash,
            }
        },
        upsert=True,
        session=session,
    )


async def pending_hosts(database: AsyncDatabase, sources: tuple[str, ...]) -> tuple[str, ...]:
    return tuple(
        await database.jobs.distinct(
            "host", {"status": "pending", "source": {"$in": list(sources)}}
        )
    )


async def claim_job(
    database: AsyncDatabase,
    sources: tuple[str, ...],
    host: str | None = None,
) -> Job | None:
    query = {"status": "pending", "source": {"$in": list(sources)}}
    if host is not None:
        query["host"] = host
    row = await database.jobs.find_one_and_update(
        query,
        {"$set": {"status": "processing"}, "$inc": {"attempts": 1}},
        sort=[("_id", 1)],
        return_document=ReturnDocument.AFTER,
    )
    return Job(row["url"], row["source"], row["etag"], row["last_modified"]) if row else None


async def complete_job(database: AsyncDatabase, result: CrawlResult) -> None:
    # Transactions require a replica set (including a single-node replica set).
    async def complete(session):
        job, page = result.job, result.page
        await enqueue_urls(database, job.source, result.links, session=session)
        if result.is_document and page.html is not None:
            await save_document(database, job.url, page.html, job.source, session=session)
        await database.jobs.update_one(
            {"url": job.url},
            {
                "$set": {
                    "status": "done",
                    "last_error": None,
                    "checked_at": int(time()),
                    "etag": page.etag,
                    "last_modified": page.last_modified,
                }
            },
            session=session,
        )

    async with database.client.start_session() as session:
        await session.with_transaction(complete)


async def block_job(database: AsyncDatabase, job: Job) -> None:
    await database.jobs.update_one(
        {"url": job.url},
        {
            "$set": {
                "status": "blocked",
                "last_error": "Blocked by robots.txt",
            }
        },
    )


async def fail_job(database: AsyncDatabase, job: Job, error: str) -> None:
    await database.jobs.update_one(
        {"url": job.url}, {"$set": {"status": "failed", "last_error": error}}
    )
