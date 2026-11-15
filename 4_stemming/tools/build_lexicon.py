import argparse
import hashlib
import json
import struct
import tarfile
import zlib
from pathlib import Path


def build(source, output):
    words = {}
    exceptions = {}
    with tarfile.open(source, "r:gz") as archive:
        for name, flag in (("noun", 1), ("verb", 2), ("adj", 4), ("adv", 8)):
            with archive.extractfile(f"WordNet-3.0/dict/index.{name}") as stream:
                for line in stream:
                    if line[:1].isspace():
                        continue
                    word = line.split()[0].decode("ascii")
                    if word.isalpha():
                        words[word] = words.get(word, 0) | flag
            with archive.extractfile(f"WordNet-3.0/dict/{name}.exc") as stream:
                for line in stream:
                    parts = line.decode("ascii").split()
                    if len(parts) < 2 or not parts[0].isalpha():
                        continue
                    candidates = [word for word in parts[1:] if word.isalpha()]
                    if candidates:
                        exceptions[(parts[0], flag)] = candidates[0]
        license_name = next(
            name
            for name in archive.getnames()
            if name in ("WordNet-3.0/COPYING", "WordNet-3.0/LICENSE")
        )
        license_text = archive.extractfile(license_name).read()
    output.parent.mkdir(parents=True, exist_ok=True)
    strings = bytearray()
    offsets = {}

    def intern(word):
        if word not in offsets:
            offsets[word] = len(strings)
            strings.extend(word.encode("ascii"))
        return offsets[word]

    entries = bytearray()
    for word, flags in sorted(words.items()):
        entries.extend(struct.pack("<IIII", intern(word), len(word), flags, 0))
    irregular = bytearray()
    for (word, flags), lemma in sorted(exceptions.items()):
        irregular.extend(
            struct.pack("<IIIII", intern(word), len(word), intern(lemma), len(lemma), flags)
        )
    payload = entries + irregular + strings
    identifier = zlib.crc32(payload)
    maximum = max(map(len, [*words, *exceptions.values()]))
    output.write_bytes(
        b"WNLEM001"
        + struct.pack("<IIIIII", len(words), len(exceptions), len(strings), maximum, identifier, 0)
        + payload
    )
    output.with_name("LICENSE.WordNet").write_bytes(license_text)
    output.with_suffix(".json").write_text(
        json.dumps(
            dict(
                version="WordNet 3.0",
                entries=len(words),
                exceptions=len(exceptions),
                maximum_length=maximum,
                dictionary_id=identifier,
                source_sha256=hashlib.file_digest(source.open("rb"), "sha256").hexdigest(),
                dictionary_sha256=hashlib.sha256(output.read_bytes()).hexdigest(),
            ),
            indent=2,
        )
        + "\n"
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    build(args.source, args.output)
