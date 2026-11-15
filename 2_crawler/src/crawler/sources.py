from html.parser import HTMLParser
from pathlib import PurePosixPath
from urllib.parse import urljoin, urlsplit

from crawler.config import Source, normalize_url
from crawler.models import CrawlResult, Job, Page, Redirect


class LinkParser(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.links: list[str] = []
        self.base: str | None = None

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        attributes = dict(attrs)
        href = attributes.get("href")

        if tag == "base" and href and self.base is None:
            self.base = href

        if tag == "a" and href:
            self.links.append(href)


class HtmlSource:
    def __init__(self, source: Source):
        self.source = source
        self.hosts = {urlsplit(url).netloc for url in source.seeds}

    def is_document(self, url: str) -> bool:
        parts = urlsplit(url)

        return (
            parts.netloc in self.hosts
            and parts.path.startswith(self.source.include_paths)
            and not parts.path.startswith(self.source.exclude_paths)
            and PurePosixPath(parts.path).suffix.lower() in {"", ".html", ".htm", ".php"}
        )

    def process_page(self, job: Job, page: Page | Redirect) -> CrawlResult:
        if isinstance(page, Redirect):
            if page.url not in self.source.seeds and not self.is_document(page.url):
                raise ValueError(f"Redirect leaves source boundaries: {job.url} -> {page.url}")

            return CrawlResult(job=job, page=Page(html=None), links=(page.url,), is_document=False)

        return CrawlResult(
            job=job,
            page=page,
            links=self.discover_links(job.url, page.html) if page.html is not None else (),
            is_document=self.is_document(job.url),
        )

    def discover_links(self, url: str, html: str) -> tuple[str, ...]:
        parser = LinkParser()
        parser.feed(html)
        base = urljoin(url, parser.base) if parser.base else url
        host = urlsplit(url).netloc
        links = {}

        for href in parser.links:
            if href.startswith("#"):
                continue

            try:
                target = normalize_url(urljoin(base, href))
            except ValueError:
                continue

            if urlsplit(target).netloc != host or target == url:
                continue

            if target in self.source.seeds or self.is_document(target):
                links[target] = None

        return tuple(links)
