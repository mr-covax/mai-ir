from bs4 import BeautifulSoup, Comment, NavigableString, Tag

from lxml import html as lxml_html

from tokenizer.models import Document
from tokenizer.profiles import SOURCES


BLOCK_TAGS = {
    "address",
    "article",
    "blockquote",
    "br",
    "dd",
    "div",
    "dl",
    "dt",
    "figure",
    "figcaption",
    "h1",
    "h2",
    "h3",
    "h4",
    "h5",
    "h6",
    "hr",
    "li",
    "ol",
    "p",
    "section",
    "table",
    "td",
    "th",
    "tr",
    "ul",
}

REMOVE_SELECTOR = ", ".join(
    (
        "script",
        "style",
        "noscript",
        "template",
        "nav",
        "aside",
        "footer",
        "svg",
        "button",
        "input",
        "select",
        ".language-selection",
        ".headerlink",
        ".viewcode-link",
        ".linenos",
        "[hidden]",
        '[aria-hidden="true"]',
    )
)


def render_text(root: Tag) -> str:
    blocks: list[str] = []
    pending: list[str] = []

    def flush() -> None:
        text = " ".join("".join(pending).split())
        pending.clear()

        if text:
            blocks.append(text)

    def visit(node: Tag | NavigableString) -> None:
        if isinstance(node, Comment):
            return

        if isinstance(node, NavigableString):
            pending.append(str(node))
            return

        if node.name == "pre":
            flush()
            code = node.get_text().strip("\n")

            if code.strip():
                blocks.append(code)

            return

        if node.name in BLOCK_TAGS:
            flush()

        for child in node.children:
            visit(child)

        if node.name in BLOCK_TAGS:
            flush()

    visit(root)
    flush()

    return "\n\n".join(blocks)


def extract_stackexchange(html: str) -> Document:
    root = lxml_html.document_fromstring(html)
    matches = root.xpath("//article")
    if not matches:
        raise ValueError("Stack Exchange article not found")
    main = matches[0]
    headings = main.xpath(".//h1")
    title = " ".join(headings[0].text_content().split()) if headings else ""
    for element in main.xpath(".//script | .//style | .//nav | .//footer"):
        element.drop_tree()
    for element in main.iter():
        if element.tag in BLOCK_TAGS or element.tag == "pre":
            element.tail = "\n\n" + (element.tail or "")
    text = main.text_content().strip()
    if not text:
        raise ValueError("Stack Exchange article is empty")
    return Document(title=title, text=text)


def select_manpage(soup: BeautifulSoup) -> Tag | None:
    heading = soup.find("h1")
    end = soup.select_one("hr.end-man-text")

    if heading is None or end is None or heading.parent is not end.parent:
        return None

    main = soup.new_tag("article")

    for node in [heading, *heading.next_siblings]:
        if node is end:
            break

        main.append(node.extract())

    return main


def extract_document(html: str, source: str = "linux-kernel") -> Document:
    if source == "stackexchange":
        return extract_stackexchange(html)

    if source not in SOURCES:
        raise ValueError(f"Unsupported source: {source}")

    rules = SOURCES[source]
    soup = BeautifulSoup(html, "html.parser")
    main = None

    if source == "man7":
        main = select_manpage(soup)
    else:
        for selector in rules.roots:
            main = soup.select_one(selector)

            if main is not None:
                break

    if main is None:
        raise ValueError(f"Main content not found for source: {source}")

    title_element = soup.select_one(rules.title) if rules.title else None
    title = " ".join(title_element.get_text().split()) if title_element is not None else None
    remove_selector = ", ".join((REMOVE_SELECTOR, *rules.remove))

    for element in list(main.select(remove_selector)):
        if element.parent is not None:
            element.decompose()

    if title is None:
        heading = main.find("h1")
        title_element = heading if heading is not None else soup.find("title")
        title = " ".join(title_element.get_text().split()) if title_element is not None else ""

    text = render_text(main)

    if not text:
        raise ValueError(f"Main content is empty for source: {source}")

    return Document(title=title, text=text)
