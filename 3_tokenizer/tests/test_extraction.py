import pytest

from tokenizer.extraction import extract_document


def page(body):
    return f'<html><head><title>Fallback title</title></head><body><div role="main">{body}</div></body></html>'


def test_extract_main_content():
    html = """
        <html><body>
        <aside>Sidebar</aside>
        <div class="body" role="main">
            <div class="language-selection">English<ul><li>Chinese</li></ul></div>
            <section>
                <h1>Kernel development<a class="headerlink">¶</a></h1>
                <p>Read the <a href="guide.html">guide</a>.</p>
                <nav>Next page</nav>
                <script>console.log('noise')</script>
                <style>.noise { display: none; }</style>
                <noscript>Enable JavaScript</noscript>
                <template>Template content</template>
                <p hidden>Hidden text</p>
                <span aria-hidden="true">Icon</span>
                <!-- Comment -->
            </section>
        </div>
        <footer>Copyright</footer>
        </body></html>
    """

    document = extract_document(html)

    assert document.title == "Kernel development"
    assert document.text == "Kernel development\n\nRead the guide."


def test_preserve_inline_identifiers_and_punctuation():
    document = extract_document(
        page(
            "<p>Use <code>std::<span>vector</span></code>, C<span>++</span> and "
            '<a href="#">pthread_create</a>().</p>'
        )
    )

    assert document.text == "Use std::vector, C++ and pthread_create()."


def test_block_boundaries():
    document = extract_document(page("<h1>Title</h1><p>First</p><p>Second<br>Third</p>"))

    assert document.text == "Title\n\nFirst\n\nSecond\n\nThird"


def test_code_indentation_and_line_breaks():
    document = extract_document(
        page("<p>Example:</p><pre><span>int</span> main() {\n    return 0;\n}\n</pre><p>End.</p>")
    )

    assert document.text == "Example:\n\nint main() {\n    return 0;\n}\n\nEnd."


def test_code_only_preserves_leading_spaces():
    assert extract_document(page("<pre>    indented();\n</pre>")).text == "    indented();"


def test_entities_unicode_and_whitespace():
    document = extract_document(page("<p>Память&nbsp;  и\nпотоки: &lt;T&gt; &amp; Linux.</p>"))

    assert document.text == "Память и потоки: <T> & Linux."


def test_lists_and_tables():
    document = extract_document(
        page(
            "<ul><li><p>First</p></li><li>Second</li></ul>"
            "<table><tr><th>Name</th><th>Value</th></tr><tr><td>Size</td><td>64</td></tr></table>"
        )
    )

    assert document.text == "First\n\nSecond\n\nName\n\nValue\n\nSize\n\n64"


def test_missing_heading_uses_page_title():
    assert extract_document(page("<p>Body</p>")).title == "Fallback title"


def test_missing_title():
    assert extract_document('<div role="main"><p>Body</p></div>').title == ""


@pytest.mark.parametrize("html", ["", "<html><body>Navigation only</body></html>"])
def test_missing_main_content(html):
    with pytest.raises(ValueError, match="Main content not found"):
        extract_document(html)


@pytest.mark.parametrize("body", ["", "  \n ", "<!-- comment -->", "<script>noise</script>"])
def test_empty_main_content(body):
    with pytest.raises(ValueError, match="Main content is empty"):
        extract_document(page(body))


def test_nested_hidden_elements():
    document = extract_document(page("<nav><span hidden>Noise</span></nav><p>Text</p>"))

    assert document.text == "Text"
