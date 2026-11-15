import asyncio
import logging

from pymongo.asynchronous.database import AsyncDatabase
import httpx

from crawler.config import Config
from crawler.http import PageLoader
from crawler.models import Redirect
from crawler.sources import HtmlSource
from crawler.storage import (
    block_job,
    claim_job,
    complete_job,
    fail_job,
    open_database,
    pending_hosts,
    prepare_queue,
)


logger = logging.getLogger(__name__)


async def crawl_host(
    host: str,
    config: Config,
    client: httpx.AsyncClient,
    database: AsyncDatabase,
    database_lock: asyncio.Lock,
) -> int:
    failures = 0
    adapters = {source.name: HtmlSource(source) for source in config.sources}
    delay = max(
        (
            adapter.source.request_delay_seconds
            if adapter.source.request_delay_seconds is not None
            else config.logic.request_delay_seconds
        )
        for adapter in adapters.values()
        if host in adapter.hosts
    )
    loader = PageLoader(client, delay)
    source_names = tuple(adapters)

    while True:
        async with database_lock:
            job = await claim_job(database, source_names, host)
        if job is None:
            return failures

        try:
            page = await loader.fetch(job)
            if page is None:
                async with database_lock:
                    await block_job(database, job)

                logger.info("Blocked by robots.txt: %s", job.url)
                continue

            result = adapters[job.source].process_page(job, page)

            async with database_lock:
                await complete_job(database, result)

            if isinstance(page, Redirect):
                logger.info("Redirected %s -> %s [%s]", job.url, page.url, job.source)
            elif page.html is None:
                logger.info("Not modified: %s [%s]", job.url, job.source)
            else:
                logger.info(
                    "Processed %s [%s], found %d links", job.url, job.source, len(result.links)
                )

        except (httpx.HTTPError, ValueError) as error:
            failures += 1

            async with database_lock:
                await fail_job(database, job, str(error))

            logger.error("Failed to fetch %s: %s", job.url, error)


async def crawl(config: Config, client: httpx.AsyncClient) -> int:
    sources = tuple(source.name for source in config.sources)
    database_lock = asyncio.Lock()

    async with open_database(config.db) as database:
        await prepare_queue(database, config.sources, config.logic.revisit_after_seconds)
        hosts = await pending_hosts(database, sources)
        tasks = [
            asyncio.create_task(
                crawl_host(host, config, client, database, database_lock), name=host
            )
            for host in hosts
        ]

        try:
            return sum(await asyncio.gather(*tasks))
        finally:
            for task in tasks:
                task.cancel()

            await asyncio.gather(*tasks, return_exceptions=True)
