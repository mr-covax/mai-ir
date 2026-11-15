from pymongo import MongoClient
from hashlib import sha256
from pathlib import Path
from tempfile import TemporaryDirectory

from tokenizer.extraction import extract_document
from tokenizer.models import ExportStats, ManifestEntry
from tokenizer.profiles import SOURCES


def export_corpus(database: str, output: Path, sources: tuple[str, ...] = ()) -> ExportStats:
    if unknown := set(sources) - SOURCES.keys():
        raise ValueError(f"Unsupported sources: {', '.join(sorted(unknown))}")

    output = output.absolute()

    if output.exists() or output.is_symlink():
        raise FileExistsError(f"Output already exists: {output}")

    query = {"source": {"$in": list(sources)}} if sources else {}
    count = 0
    text_bytes = 0

    with (
        MongoClient(database, serverSelectionTimeoutMS=5000) as client,
        client.start_session(snapshot=True) as session,
    ):
        connection = client.get_default_database()
        if "documents" not in connection.list_collection_names():
            raise ValueError("Crawler documents collection does not exist")
        output.parent.mkdir(parents=True, exist_ok=True)

        with TemporaryDirectory(prefix=f".{output.name}-", dir=output.parent) as temporary:
            staging = Path(temporary)
            (staging / "texts").mkdir()

            with (staging / "manifest.jsonl").open("w", encoding="utf-8", newline="\n") as manifest:
                for row in connection.documents.find(query, session=session).sort("url", 1):
                    url, source, html = row["url"], row["source"], row["html"]
                    try:
                        document = extract_document(html, source)
                    except ValueError as error:
                        raise ValueError(f"Failed to extract {url} [{source}]: {error}") from error

                    identifier = sha256(url.encode("utf-8")).hexdigest()
                    path = f"texts/{identifier}.txt"
                    text = (document.text + "\n").encode("utf-8")
                    entry = ManifestEntry(
                        id=identifier,
                        url=url,
                        source=source,
                        title=document.title,
                        path=path,
                        text_bytes=len(text),
                    )

                    (staging / path).write_bytes(text)
                    manifest.write(entry.model_dump_json(exclude_none=True) + "\n")
                    count += 1
                    text_bytes += len(text)

            if output.exists() or output.is_symlink():
                raise FileExistsError(f"Output already exists: {output}")

            staging.rename(output)

    return ExportStats(documents=count, text_bytes=text_bytes)
