from pydantic import BaseModel, ConfigDict


class ExtractionRules(BaseModel):
    model_config = ConfigDict(frozen=True, extra="forbid")

    roots: tuple[str, ...]
    remove: tuple[str, ...] = ()
    title: str | None = None


SPHINX = ExtractionRules(
    roots=('[role="main"]',),
    remove=(".toctree-wrapper", ".related", ".sphinxsidebar", ".contents.topic"),
)

GNU = ExtractionRules(
    roots=(".contents", "body"),
    remove=(
        ".nav-panel",
        ".header",
        ".menu",
        ".index",
        ".element-contents",
        ".element-shortcontents",
        ".copiable-link",
        ".anchor",
        ".summary",
        ".headertitle",
        ".directory",
    ),
    title=".header .title",
)

FREEBSD = ExtractionRules(
    roots=("main .book", "main .article", "main .document", "main"),
    remove=(".toc", ".toc-mobile", ".book-menu", ".book-menu-mobile", ".book-footer", ".edit"),
)

CPPREFERENCE = ExtractionRules(
    roots=("#mw-content-text .mw-parser-output", "#mw-content-text"),
    remove=(".t-navbar", "#toc", ".toc", ".mw-editsection", ".printfooter", ".catlinks"),
    title="#firstHeading",
)

MAN7 = ExtractionRules(roots=(), remove=(".sec-table", ".top-link"))

SOURCES = {
    "stackexchange": ExtractionRules(roots=("article",)),
    "linux-kernel": SPHINX,
    "llvm": SPHINX,
    "clang": SPHINX.model_copy(update={"roots": ('[role="main"]', "#content")}),
    "lld": SPHINX,
    "lldb": SPHINX.model_copy(update={"roots": ('[role="main"]', "#doc-content")}),
    "llvm-libc": SPHINX,
    "llvm-libcxx": SPHINX,
    "gcc": GNU,
    "glibc": GNU,
    "binutils": GNU,
    "gdb": GNU,
    "freebsd-docs": FREEBSD,
    "cppreference": CPPREFERENCE,
    "man7": MAN7,
}
