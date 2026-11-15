from urllib.parse import urlsplit
from urllib.robotparser import RobotFileParser

import httpx


USER_AGENT = "CourseCrawler/0.1"


def robots_url(url: str) -> str:
    parts = urlsplit(url)
    return f"{parts.scheme}://{parts.netloc}/robots.txt"


async def load_robots(client: httpx.AsyncClient, url: str) -> RobotFileParser:
    response = await client.get(url, follow_redirects=False)
    rules = RobotFileParser(url)

    if response.status_code in {404, 410}:
        rules.parse([])
    elif response.status_code in {401, 403}:
        rules.parse(["User-agent: *", "Disallow: /"])
    else:
        response.raise_for_status()
        rules.parse(response.text.splitlines())

    return rules


def request_delay(rules: RobotFileParser, configured_delay: float) -> float:
    delay = max(configured_delay, rules.crawl_delay(USER_AGENT) or 0)
    rate = rules.request_rate(USER_AGENT)

    if rate and rate.requests > 0:
        delay = max(delay, rate.seconds / rate.requests)

    return delay
