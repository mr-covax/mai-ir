import asyncio

from crawler.config import DatabaseConfig, Source
from crawler.storage import claim_job, open_database, pending_hosts, prepare_queue


async def test_host_is_stored_and_used_for_claiming(mongo_config):
    async with open_database(DatabaseConfig(**mongo_config)) as database:
        await prepare_queue(
            database,
            (
                Source(
                    name="test",
                    seeds=(
                        "https://first.org/page",
                        "https://second.org:8443/page",
                    ),
                ),
            ),
        )

        assert set(await pending_hosts(database, ("test",))) == {"first.org", "second.org:8443"}
        assert (
            await claim_job(database, ("test",), "second.org:8443")
        ).url == "https://second.org:8443/page"
        assert await claim_job(database, ("test",), "second.org:8443") is None
        assert (await claim_job(database, ("test",), "first.org")).url == "https://first.org/page"


async def test_concurrent_claim_is_atomic(mongo_config):
    async with open_database(DatabaseConfig(**mongo_config)) as database:
        await prepare_queue(database, (Source(name="test", seeds=("https://example.org/",)),))
        results = await asyncio.gather(*(claim_job(database, ("test",)) for _ in range(10)))
        assert sum(job is not None for job in results) == 1
        assert (await database.jobs.find_one())["attempts"] == 1
