from pydantic import BaseModel, ConfigDict, Field


class Model(BaseModel):
    model_config = ConfigDict(frozen=True, extra="forbid", allow_inf_nan=False)


class Document(Model):
    title: str
    text: str


class ManifestEntry(Model):
    id: str
    url: str
    source: str
    title: str
    path: str
    text_bytes: int
    text_offset: int | None = Field(default=None, ge=0)


class ExportStats(Model):
    documents: int
    text_bytes: int


class TokenStatistics(Model):
    tokens: int = Field(strict=True, ge=0)
    total_length: int = Field(strict=True, ge=0)
    average_length: float = Field(ge=0)
    input_bytes: int = Field(strict=True, ge=0)
    elapsed_seconds: float = Field(ge=0)
    seconds_per_kib: float = Field(ge=0)
    mib_per_second: float = Field(ge=0)


class CorpusRun(Model):
    documents: int
    tokens: int
    total_length: int
    average_length: float
    input_bytes: int
    tokenization_seconds: float
    wall_seconds: float
    overhead_seconds: float
    seconds_per_kib: float
    mib_per_second: float


class CorpusReport(Model):
    runs: tuple[CorpusRun, ...]
    median_tokenization_seconds: float
    median_wall_seconds: float
