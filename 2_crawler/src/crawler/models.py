from dataclasses import dataclass


@dataclass(frozen=True)
class Job:
    url: str
    source: str
    etag: str | None = None
    last_modified: str | None = None


@dataclass(frozen=True)
class Page:
    html: str | None
    etag: str | None = None
    last_modified: str | None = None


@dataclass(frozen=True)
class Redirect:
    url: str


@dataclass(frozen=True)
class CrawlResult:
    job: Job
    page: Page
    links: tuple[str, ...] = ()
    is_document: bool = True
