import asyncio
from time import monotonic
from urllib.parse import urljoin, urlsplit
from urllib.robotparser import RobotFileParser

import httpx

from crawler.config import normalize_url
from crawler.models import Job, Page, Redirect
from crawler.robots import USER_AGENT, load_robots, request_delay, robots_url


async def wait_for_host(url: str, next_request: dict[str, float], delay: float) -> None:
    host = urlsplit(url).netloc
    wait = next_request.get(host, 0) - monotonic()

    if wait > 0:
        await asyncio.sleep(wait)

    next_request[host] = monotonic() + delay


def resolve_redirect(url: str, location: str | None) -> Redirect:
    if not location:
        raise ValueError("Redirect response has no Location header")

    target = normalize_url(urljoin(url, location))
    origin = urlsplit(url)
    destination = urlsplit(target)

    if destination.netloc != origin.netloc:
        raise ValueError(f"Cross-host redirect is not allowed: {url} -> {target}")

    if origin.scheme == "https" and destination.scheme == "http":
        target = destination._replace(scheme="https").geturl()

    if target == url:
        raise ValueError(f"Redirect points to the same URL: {url}")

    return Redirect(url=target)


class PageLoader:
    def __init__(self, client: httpx.AsyncClient, delay: float):
        self.client = client
        self.delay = delay
        self.next_request: dict[str, float] = {}
        self.policies: dict[str, RobotFileParser | httpx.HTTPError] = {}

    async def policy_for(self, url: str) -> RobotFileParser:
        policy_url = robots_url(url)

        if policy_url not in self.policies:
            await wait_for_host(policy_url, self.next_request, self.delay)

            try:
                policy = await load_robots(self.client, policy_url)
            except httpx.HTTPError as error:
                self.policies[policy_url] = error
            else:
                self.policies[policy_url] = policy
                host = urlsplit(url).netloc
                self.next_request[host] += max(0, request_delay(policy, self.delay) - self.delay)

        policy = self.policies[policy_url]

        if isinstance(policy, httpx.HTTPError):
            raise policy

        return policy

    async def fetch(self, job: Job) -> Page | Redirect | None:
        url = job.url
        policy = await self.policy_for(url)

        if not policy.can_fetch(USER_AGENT, url):
            return None

        await wait_for_host(url, self.next_request, request_delay(policy, self.delay))
        headers = {}

        if job.etag:
            headers["If-None-Match"] = job.etag

        if job.last_modified:
            headers["If-Modified-Since"] = job.last_modified

        response = await self.client.get(url, headers=headers, follow_redirects=False)

        if response.status_code in {301, 302, 303, 307, 308}:
            return resolve_redirect(url, response.headers.get("location"))

        if response.status_code == 304:
            if not headers:
                raise ValueError("Received 304 without a conditional request")

            return Page(
                html=None,
                etag=response.headers.get("etag", job.etag),
                last_modified=response.headers.get("last-modified", job.last_modified),
            )

        response.raise_for_status()

        content_type = response.headers.get("content-type", "").split(";", 1)[0].strip().lower()

        if content_type not in {"text/html", "application/xhtml+xml"}:
            raise ValueError(f"Expected HTML, received {content_type or 'unknown content type'}")

        return Page(
            html=response.text,
            etag=response.headers.get("etag"),
            last_modified=response.headers.get("last-modified"),
        )
