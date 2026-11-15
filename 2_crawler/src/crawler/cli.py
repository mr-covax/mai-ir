import argparse
import asyncio
import logging
from pathlib import Path

from pymongo.errors import PyMongoError
import httpx

from crawler.config import load_config
from crawler.engine import crawl
from crawler.robots import USER_AGENT


async def run(path: Path) -> int:
    config = load_config(path)

    async with httpx.AsyncClient(
        timeout=config.logic.timeout_seconds,
        headers={"User-Agent": USER_AGENT},
    ) as client:
        return int(await crawl(config, client) > 0)


def main() -> None:
    parser = argparse.ArgumentParser(description="Fetch documentation into MongoDB")
    parser.add_argument("config", type=Path)
    arguments = parser.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")

    try:
        status = asyncio.run(run(arguments.config))
    except (OSError, ValueError, PyMongoError) as error:
        logging.error("%s", error)
        status = 1
    except KeyboardInterrupt:
        status = 130

    raise SystemExit(status)
