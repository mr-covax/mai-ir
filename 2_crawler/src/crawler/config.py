from pathlib import Path
from typing import Annotated
from urllib.parse import urlsplit

import yaml
from pydantic import BaseModel, ConfigDict, Field, StringConstraints, field_validator


Name = Annotated[str, StringConstraints(strip_whitespace=True, min_length=1)]


def normalize_url(value: str) -> str:
    url = urlsplit(value)

    if url.scheme not in {"http", "https"} or not url.hostname:
        raise ValueError(f"Expected an HTTP(S) URL: {value}")

    if url.username is not None or url.password is not None:
        raise ValueError("URL must not contain credentials")

    host = url.hostname.encode("idna").decode().lower()
    host = f"[{host}]" if ":" in host else host
    port = url.port

    if port is not None and (url.scheme, port) not in {("http", 80), ("https", 443)}:
        host = f"{host}:{port}"

    return url._replace(netloc=host, path=url.path or "/", fragment="").geturl()


class Model(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True, allow_inf_nan=False)


class DatabaseConfig(Model):
    uri: Name
    name: Name

    @field_validator("uri")
    @classmethod
    def validate_uri(cls, value: str) -> str:
        from pymongo.errors import PyMongoError
        from pymongo.uri_parser import parse_uri

        try:
            parse_uri(value)
        except PyMongoError as error:
            raise ValueError(str(error)) from error
        return value

    @field_validator("name")
    @classmethod
    def validate_name(cls, value: str) -> str:
        if any(character in value for character in '/\\. "$\x00'):
            raise ValueError("Invalid MongoDB database name")
        return value


class LogicConfig(Model):
    request_delay_seconds: float = Field(strict=True, ge=0)
    timeout_seconds: float = Field(strict=True, gt=0)
    revisit_after_seconds: float = Field(default=86400, strict=True, ge=0)


class Source(Model):
    name: Name
    seeds: tuple[str, ...] = Field(min_length=1)
    include_paths: tuple[str, ...] = Field(default=("/",), min_length=1)
    exclude_paths: tuple[str, ...] = ()
    request_delay_seconds: Annotated[float, Field(strict=True, ge=0)] | None = None

    @field_validator("include_paths", "exclude_paths")
    @classmethod
    def validate_paths(cls, paths: tuple[str, ...]) -> tuple[str, ...]:
        if any(not path.startswith("/") or "?" in path or "#" in path for path in paths):
            raise ValueError("Path rules must start with / and contain no query or fragment")

        return paths

    @field_validator("seeds")
    @classmethod
    def normalize_seeds(cls, seeds: tuple[str, ...]) -> tuple[str, ...]:
        return tuple(dict.fromkeys(normalize_url(url) for url in seeds))


class Config(Model):
    db: DatabaseConfig
    logic: LogicConfig
    sources: tuple[Source, ...] = Field(min_length=1)

    @field_validator("sources")
    @classmethod
    def unique_sources(cls, sources: tuple[Source, ...]) -> tuple[Source, ...]:
        names = [source.name for source in sources]

        if len(names) != len(set(names)):
            raise ValueError("Source names must be unique")

        return sources


def load_config(path: Path) -> Config:
    try:
        data = yaml.safe_load(path.read_text(encoding="utf-8"))
    except yaml.YAMLError as error:
        raise ValueError(f"Invalid YAML: {path}") from error

    return Config.model_validate(data)
