import argparse
import html
import json
import subprocess
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlencode, urlsplit


def serve(binary, index, host, port):
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            address = urlsplit(self.path)
            if address.path not in {"/", "/search"}:
                self.send_error(404)
                return
            arguments = parse_qs(address.query)
            query = arguments.get("q", [""])[0]
            try:
                page = int(arguments.get("page", ["0"])[0])
                if (
                    page < 0
                    or page > 1000000
                    or len(query.encode()) > 4096
                    or any(c in query for c in "\r\n\0")
                ):
                    raise ValueError("Некорректный запрос или номер страницы")
            except ValueError:
                self.send_error(400)
                return
            results = ""
            status = 200
            if query:
                try:
                    response = subprocess.run(
                        [str(binary), str(index), "50", str(page * 50)],
                        input=query + "\n",
                        capture_output=True,
                        text=True,
                        encoding="utf-8",
                        timeout=30,
                    )
                    data = json.loads(response.stdout)
                    if response.returncode:
                        status = 400
                        results = f"<p>{html.escape(data.get('error', 'Ошибка запроса'))}</p>"
                    else:
                        results = f'<p>Найдено: {data["total"]}</p><ol start="{page * 50 + 1}">'
                        for item in data["results"]:
                            if urlsplit(item["url"]).scheme in {"https", "http"}:
                                results += f'<li><a href="{html.escape(item["url"], quote=True)}">{html.escape(item["title"] or item["url"])}</a></li>'
                        results += "</ol>"
                        if (page + 1) * 50 < data["total"]:
                            results += f'<a href="/search?{html.escape(urlencode(dict(q=query, page=page + 1)), quote=True)}">Следующие 50</a>'
                except (OSError, ValueError, subprocess.TimeoutExpired):
                    self.send_error(500, "Search failed")
                    return
            content = f'''<!doctype html><html lang="ru"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Поиск по документации</title><style>body{{font:17px system-ui;max-width:850px;margin:50px auto;padding:0 20px}}input,button{{font:inherit;padding:10px}}input{{width:65%}}li{{margin:22px 0}}</style>
<h1>Поиск по документации</h1><form action="/search"><input name="q" aria-label="Запрос" value="{html.escape(query, quote=True)}"><button>Найти</button></form>
<p>Операторы: &amp;&amp;, ||, ! и скобки. Пробел означает И.</p>{results}</html>'''.encode()
            self.send_response(status)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(content)))
            self.end_headers()
            self.wfile.write(content)

    ThreadingHTTPServer((host, port), Handler).serve_forever()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("index", type=Path)
    parser.add_argument("--binary", type=Path, default=Path("build/boolean"))
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()
    serve(args.binary.resolve(strict=True), args.index.resolve(strict=True), args.host, args.port)
