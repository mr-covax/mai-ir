import html
import json
import subprocess
from configparser import ConfigParser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit


def serve(binary, host, port):
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            address = urlsplit(self.path)
            if address.path not in {"/", "/search"}:
                self.send_error(404)
                return

            arguments = parse_qs(address.query)
            query = arguments.get("q", [""])[0]

            if len(query.encode()) > 4096 or any(c in query for c in "\r\n\0"):
                self.send_error(400)
                return

            results = ""
            status = 200

            if query:
                try:
                    response = subprocess.run(
                        [str(binary), "search"],
                        input=query + "\n",
                        capture_output=True,
                        text=True,
                        encoding="utf-8",
                        timeout=30,
                    )
                    data = json.loads(response.stdout)

                    if response.returncode:
                        status = 400
                        results = (
                            f"<p>{html.escape(data.get('error', 'Ошибка запроса'))}</p>"
                        )
                    else:
                        offset = data["offset"]
                        results = (
                            f'<p>Найдено: {data["total"]}</p><ol start="{offset + 1}">'
                        )

                        for item in data["results"]:
                            if urlsplit(item["url"]).scheme in {"https", "http"}:
                                url = html.escape(item["url"], quote=True)
                                title = html.escape(item["title"] or item["url"])
                                results += f'<li><a href="{url}">{title}</a></li>'

                        results += "</ol>"
                except (OSError, ValueError, subprocess.TimeoutExpired):
                    self.send_error(500, "Search failed")
                    return

            content = f'''<!doctype html><html lang="ru"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Поиск по документации</title><style>body{{font:17px system-ui;max-width:850px;margin:50px auto;padding:0 20px}}input,button{{font:inherit;padding:10px}}input{{width:65%}}li{{margin:22px 0}}</style>
<h1>Поиск по документации</h1><form action="/search"><input name="q" aria-label="Запрос" value="{html.escape(query, quote=True)}"><button>Найти</button></form>
<p>Операторы: &amp;&amp;, ||, !, скобки и цитаты в кавычках; / N задаёт расстояние. В ранжированном поиске обычные слова допускают частичное совпадение.</p>{results}</html>'''.encode()

            self.send_response(status)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(content)))
            self.end_headers()
            self.wfile.write(content)

    with ThreadingHTTPServer((host, port), Handler) as server:
        print(f"Поиск запущен на http://{host}:{server.server_port}", flush=True)
        server.serve_forever()


if __name__ == "__main__":
    config = ConfigParser(interpolation=None)
    config.read_string("[lab]\n" + Path("config").read_text(encoding="utf-8"))
    settings = config["lab"]
    host = settings["host"]
    port = settings.getint("port")

    if not 0 <= port <= 65535:
        raise ValueError("port must be between 0 and 65535")

    serve(Path("build/positional").resolve(strict=True), host, port)
