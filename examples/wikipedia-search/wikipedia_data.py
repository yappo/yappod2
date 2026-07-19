#!/usr/bin/env python3
"""Download Japanese Wikipedia samples and produce yappod canonical NDJSON."""

import argparse
import bz2
import contextlib
from datetime import datetime, timezone
import gzip
import hashlib
import ipaddress
import json
import math
import os
from pathlib import Path
import re
import shlex
import sys
import tempfile
import time
from typing import Dict, Iterable, Iterator, List, NamedTuple, Optional, TextIO, Tuple
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode, urlsplit
from urllib.request import Request, urlopen

try:
    import tomllib
except ImportError:  # Python 3.9 and 3.10
    tomllib = None


DEFAULT_API_URL = "https://ja.wikipedia.org/w/api.php"
DEFAULT_DUMP_BASE_URL = "https://dumps.wikimedia.org/jawiki/latest"
DUMP_FILENAME = "jawiki-latest-pages-articles-multistream.xml.bz2"
CHECKSUM_FILENAME = "jawiki-latest-sha1sums.txt"
DUMP_CHECKSUM_PATTERN = re.compile(r"^jawiki-\d{8}-pages-articles-multistream\.xml\.bz2$")
API_SEARCH_LIMIT = 50
DEFAULT_USER_AGENT = "yappod2-wikipedia-example/1.0 (https://github.com/yappo/yappod2)"
EXAMPLE_DIR = Path(__file__).resolve().parent
DEFAULT_APP_CONFIG = EXAMPLE_DIR / "wikipedia-search.toml"
DEFAULT_TOPICS = (
    "日本の歴史",
    "日本の地理",
    "自然科学",
    "情報技術",
    "文学",
    "芸術",
    "音楽",
    "映画",
    "スポーツ",
    "食文化",
    "医学",
    "教育",
    "経済",
    "法律",
    "建築",
    "交通",
    "宇宙",
    "生物",
    "環境",
    "言語",
)


class WikipediaDataError(Exception):
    """A user-facing failure that should not publish partial output."""


class FriendlyArgumentParser(argparse.ArgumentParser):
    """Argument parser that reports the command needed to inspect valid input."""

    def error(self, message: str) -> None:
        self.print_usage(sys.stderr)
        help_command = "{} --help".format(self.prog)
        self.exit(
            2,
            "wikipedia-data: error: invalid command arguments\n"
            "Reason: {}\n"
            "How to fix:\n"
            "  1. Run `{}` to see the accepted arguments.\n"
            "  2. Correct the command and try again.\n".format(message, help_command),
        )


class EmbeddingSettings(NamedTuple):
    provider: str
    base_url: Optional[str]
    endpoint_url: str
    model: str
    dimensions: int
    batch_size: int
    timeout: float
    profile: str
    authorization_token: Optional[str]
    usage_log_path: Optional[Path]


def _strip_toml_comment(line: str) -> str:
    quote: Optional[str] = None
    escaped = False
    for index, character in enumerate(line):
        if quote == '"' and escaped:
            escaped = False
            continue
        if quote == '"' and character == "\\":
            escaped = True
            continue
        if character in ("'", '"'):
            quote = None if quote == character else character if quote is None else quote
        elif character == "#" and quote is None:
            return line[:index]
    return line


def _fallback_toml_value(value: str, path: Path, line_number: int) -> object:
    if value.startswith('"') and value.endswith('"'):
        try:
            return json.loads(value)
        except json.JSONDecodeError as error:
            raise WikipediaDataError("invalid TOML string at {}:{}".format(path, line_number)) from error
    if value.startswith("'") and value.endswith("'"):
        return value[1:-1]
    if value == "true":
        return True
    if value == "false":
        return False
    if re.fullmatch(r"[+-]?[0-9][0-9_]*", value):
        return int(value.replace("_", ""))
    raise WikipediaDataError("unsupported TOML value at {}:{}".format(path, line_number))


def _fallback_toml_table(source: str, path: Path, table_name: str) -> Dict[str, object]:
    current_table: Optional[str] = None
    result: Dict[str, object] = {}
    for line_number, raw_line in enumerate(source.splitlines(), 1):
        line = _strip_toml_comment(raw_line).strip()
        if not line:
            continue
        table_match = re.fullmatch(r"\[([A-Za-z0-9_.-]+)\]", line)
        if table_match:
            current_table = table_match.group(1)
            continue
        if current_table != table_name:
            continue
        assignment = re.fullmatch(r"([A-Za-z0-9_-]+)\s*=\s*(.+)", line)
        if not assignment:
            raise WikipediaDataError("invalid TOML assignment at {}:{}".format(path, line_number))
        key = assignment.group(1)
        if key in result:
            raise WikipediaDataError("duplicate TOML key {}.{}".format(table_name, key))
        result[key] = _fallback_toml_value(assignment.group(2).strip(), path, line_number)
    return result


def _read_toml_table(path: Path, table_name: str, required: bool = True) -> Dict[str, object]:
    try:
        if tomllib is not None:
            with path.open("rb") as source:
                document = tomllib.load(source)
            value = document.get(table_name)
        else:
            value = _fallback_toml_table(path.read_text(encoding="utf-8"), path, table_name)
    except (OSError, UnicodeDecodeError) as error:
        raise WikipediaDataError("cannot read TOML config {}: {}".format(path, error)) from error
    except Exception as error:
        if tomllib is not None and isinstance(error, tomllib.TOMLDecodeError):
            raise WikipediaDataError("invalid TOML config {}: {}".format(path, error)) from error
        raise
    if not required and (value is None or value == {}):
        return {}
    if not isinstance(value, dict) or not value:
        raise WikipediaDataError("{} must contain a [{}] table".format(path, table_name))
    return value


def _config_string(table: Dict[str, object], key: str, name: str, required: bool = True) -> Optional[str]:
    value = table.get(key)
    if value is None and not required:
        return None
    if not isinstance(value, str) or not value.strip():
        raise WikipediaDataError("{} must be a non-empty string".format(name))
    return value.strip()


def _config_integer(
    table: Dict[str, object], key: str, name: str, default: Optional[int], minimum: int, maximum: int
) -> int:
    value = table.get(key, default)
    if not isinstance(value, int) or isinstance(value, bool) or value < minimum or value > maximum:
        raise WikipediaDataError("{} must be an integer from {} to {}".format(name, minimum, maximum))
    return value


def _only_config_keys(table: Dict[str, object], allowed: Tuple[str, ...], name: str) -> None:
    unknown = next((key for key in table if key not in allowed), None)
    if unknown is not None:
        raise WikipediaDataError("{} contains unknown key: {}".format(name, unknown))


_HTTP_NETWORKS = tuple(
    ipaddress.ip_network(value)
    for value in (
        "127.0.0.0/8", "10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16",
        "::1/128", "fc00::/7",
    )
)


def _service_url(value: object, name: str, base: bool = False) -> str:
    result = _config_string({"value": value}, "value", name)
    assert result is not None
    parsed = urlsplit(result)
    if parsed.scheme not in ("http", "https") or not parsed.netloc or parsed.hostname is None:
        raise WikipediaDataError("{} must be an http or https URL".format(name))
    if parsed.scheme == "http":
        host = parsed.hostname.rstrip(".").lower()
        allowed = host == "localhost"
        if not allowed:
            try:
                address = ipaddress.ip_address(host)
                allowed = any(address.version == network.version and address in network for network in _HTTP_NETWORKS)
            except ValueError:
                allowed = False
        if not allowed:
            raise WikipediaDataError(
                "{} must use https outside localhost, loopback, or private networks".format(name)
            )
    if base and (parsed.query or parsed.fragment):
        raise WikipediaDataError("{} must not contain a query or fragment".format(name))
    return result.rstrip("/") if base else result


def _authorization_token_from_env(table: Dict[str, object], name: str) -> Optional[str]:
    if "authorization_token" in table:
        raise WikipediaDataError(
            "{}.authorization_token is not supported; use {}.authorization_token_env".format(name, name)
        )
    environment_name = _config_string(
        table, "authorization_token_env", "{}.authorization_token_env".format(name), required=False
    )
    if environment_name is None:
        return None
    if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", environment_name) is None:
        raise WikipediaDataError("{}.authorization_token_env must be an environment variable name".format(name))
    token = os.environ.get(environment_name)
    if token is None or not token.strip():
        raise WikipediaDataError("environment variable {} is not set or empty".format(environment_name))
    return token.strip()


def _usage_log_path(web_config: Path) -> Optional[Path]:
    usage_log = _read_toml_table(web_config, "usage_log", required=False)
    if not usage_log:
        return None
    _only_config_keys(usage_log, ("path",), "usage_log")
    value = _config_string(usage_log, "path", "usage_log.path")
    assert value is not None
    path = Path(value).expanduser()
    if not path.is_absolute():
        path = web_config.resolve().parent / path
    return path.resolve(strict=False)


def _append_usage_log(path: Optional[Path], provider: str, model: str, usage: object) -> None:
    if path is None:
        return
    record = {
        "timestamp": datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z"),
        "source": "wikipedia-data",
        "service": "embedding",
        "operation": "batch_embed",
        "provider": provider,
        "model": model,
        "usage": usage if isinstance(usage, dict) else None,
    }
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("a", encoding="utf-8") as output:
            output.write(json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n")
    except OSError as error:
        print("warning: cannot append usage log {}: {}".format(path, error), file=sys.stderr)


def load_embedding_settings(app_config: Path) -> EmbeddingSettings:
    vector = _read_toml_table(app_config, "vector")
    embedding = _read_toml_table(app_config, "embedding")
    _only_config_keys(vector, ("enabled", "model_id", "dimensions", "metric"), "vector")
    _only_config_keys(embedding, (
        "directory", "provider", "base_url", "endpoint_url", "model", "model_id", "dimensions",
        "prompt_profile", "authorization_token", "authorization_token_env", "timeout_ms", "batch_size",
    ), "embedding")
    if vector.get("enabled") is not True:
        raise WikipediaDataError("vector.enabled must be true in {}".format(app_config))
    index_model_id = _config_string(vector, "model_id", "vector.model_id")
    dimensions = _config_integer(vector, "dimensions", "vector.dimensions", None, 1, 65536)
    configured_dimensions = embedding.get("dimensions", dimensions)
    if configured_dimensions != dimensions:
        raise WikipediaDataError("embedding.dimensions must match vector.dimensions")
    configured_model_id = _config_string(
        embedding, "model_id", "embedding.model_id", required=False
    )
    if configured_model_id is not None and configured_model_id != index_model_id:
        raise WikipediaDataError("embedding.model_id must match vector.model_id")
    provider = _config_string(embedding, "provider", "embedding.provider")
    if provider not in ("lmstudio", "ollama", "openai"):
        raise WikipediaDataError("embedding.provider must be lmstudio, ollama, or openai")
    base_value = embedding.get("base_url")
    endpoint_value = embedding.get("endpoint_url")
    if (base_value is None) == (endpoint_value is None):
        raise WikipediaDataError("embedding must specify exactly one of base_url or endpoint_url")
    base_url = _service_url(base_value, "embedding.base_url", base=True) if base_value is not None else None
    if endpoint_value is not None:
        endpoint_url = _service_url(endpoint_value, "embedding.endpoint_url")
    else:
        assert base_url is not None
        endpoint_url = base_url + ("/api/embed" if provider == "ollama" else "/embeddings")
    profile = _config_string(
        embedding, "prompt_profile", "embedding.prompt_profile", required=False
    ) or "plain"
    if profile not in ("embeddinggemma", "plain"):
        raise WikipediaDataError("embedding.prompt_profile must be embeddinggemma or plain")
    authorization_token = _authorization_token_from_env(embedding, "embedding")
    timeout_ms = _config_integer(embedding, "timeout_ms", "embedding.timeout_ms", 60000, 1000, 600000)
    return EmbeddingSettings(
        provider,
        base_url,
        endpoint_url,
        _config_string(embedding, "model", "embedding.model"),
        dimensions,
        _config_integer(embedding, "batch_size", "embedding.batch_size", 16, 1, 1024),
        timeout_ms / 1000.0,
        profile,
        authorization_token,
        _usage_log_path(app_config),
    )


def _request_bytes(
    url: str,
    user_agent: str,
    retries: int = 4,
    timeout: float = 30.0,
) -> bytes:
    for attempt in range(retries + 1):
        request = Request(url, headers={"User-Agent": user_agent, "Accept": "application/json"})
        try:
            with urlopen(request, timeout=timeout) as response:
                return response.read()
        except HTTPError as error:
            retryable = error.code == 429 or error.code >= 500
            if not retryable or attempt == retries:
                raise WikipediaDataError("HTTP {} while requesting {}".format(error.code, url)) from error
            retry_after = error.headers.get("Retry-After")
            delay = float(retry_after) if retry_after and retry_after.isdigit() else 2 ** attempt
        except (URLError, TimeoutError, OSError) as error:
            if attempt == retries:
                raise WikipediaDataError("request failed for {}: {}".format(url, error)) from error
            delay = 2 ** attempt
        time.sleep(delay)
    raise AssertionError("unreachable")


def _request_json(url: str, user_agent: str) -> Dict[str, object]:
    try:
        value = json.loads(_request_bytes(url, user_agent).decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise WikipediaDataError("Wikimedia API returned invalid UTF-8 JSON") from error
    if not isinstance(value, dict):
        raise WikipediaDataError("Wikimedia API response root must be an object")
    if "error" in value:
        raise WikipediaDataError("Wikimedia API returned an error: {}".format(value["error"]))
    return value


def _post_json(url: str, payload: Dict[str, object], timeout: float, api_key: Optional[str]) -> Dict[str, object]:
    headers = {"Content-Type": "application/json", "Accept": "application/json"}
    if api_key:
        headers["Authorization"] = "Bearer " + api_key
    request = Request(
        url,
        data=json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8"),
        headers=headers,
        method="POST",
    )
    try:
        with urlopen(request, timeout=timeout) as response:
            value = json.loads(response.read().decode("utf-8"))
    except HTTPError as error:
        raise WikipediaDataError("embedding server returned HTTP {}".format(error.code)) from error
    except (URLError, TimeoutError, OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise WikipediaDataError("embedding request failed: {}".format(error)) from error
    if not isinstance(value, dict):
        raise WikipediaDataError("embedding response root must be an object")
    return value


def _validated_vector(value: object, dimensions: int) -> List[float]:
    if not isinstance(value, list) or len(value) != dimensions:
        raise WikipediaDataError("embedding vector must contain {} values".format(dimensions))
    if any(not isinstance(item, (int, float)) or isinstance(item, bool) or not math.isfinite(item) for item in value):
        raise WikipediaDataError("embedding vector contains a non-finite number")
    return [float(item) for item in value]


def _embedding_batch(
    provider: str,
    endpoint_url: str,
    model: str,
    inputs: List[str],
    dimensions: int,
    timeout: float,
    api_key: Optional[str],
    usage_log_path: Optional[Path] = None,
) -> List[List[float]]:
    payload: Dict[str, object] = {"model": model, "input": inputs}
    if provider == "openai":
        payload["dimensions"] = dimensions
    response = _post_json(endpoint_url, payload, timeout, api_key)
    _append_usage_log(usage_log_path, provider, model, response.get("usage"))
    if provider == "ollama":
        embeddings = response.get("embeddings")
        if not isinstance(embeddings, list) or len(embeddings) != len(inputs):
            raise WikipediaDataError("Ollama embedding count does not match the input count")
        return [_validated_vector(item, dimensions) for item in embeddings]
    data = response.get("data")
    if not isinstance(data, list) or len(data) != len(inputs):
        raise WikipediaDataError("OpenAI-compatible embedding count does not match the input count")
    ordered: List[Optional[List[float]]] = [None] * len(inputs)
    for item in data:
        if not isinstance(item, dict):
            raise WikipediaDataError("OpenAI-compatible embedding item must be an object")
        index = item.get("index")
        if not isinstance(index, int) or isinstance(index, bool) or index < 0 or index >= len(inputs) or ordered[index] is not None:
            raise WikipediaDataError("OpenAI-compatible embedding indexes are invalid")
        ordered[index] = _validated_vector(item.get("embedding"), dimensions)
    if any(item is None for item in ordered):
        raise WikipediaDataError("OpenAI-compatible embedding response has missing indexes")
    return [item for item in ordered if item is not None]


def _read_ndjson(path: Path, label: str) -> List[Dict[str, object]]:
    records: List[Dict[str, object]] = []
    try:
        with path.open("r", encoding="utf-8") as source:
            for line_number, line in enumerate(source, 1):
                if not line.strip():
                    continue
                try:
                    value = json.loads(line)
                except json.JSONDecodeError as error:
                    raise WikipediaDataError("invalid {} JSON at {}:{}".format(label, path, line_number)) from error
                if not isinstance(value, dict):
                    raise WikipediaDataError("{} record must be an object".format(label))
                records.append(value)
    except (UnicodeDecodeError, OSError) as error:
        raise WikipediaDataError("cannot read {}: {}".format(path, error)) from error
    return records


def embed_documents(
    documents_path: Path,
    passages_path: Path,
    output_path: Path,
    provider: str,
    endpoint_url: str,
    model: str,
    dimensions: int,
    batch_size: int,
    timeout: float,
    profile: str,
    api_key: Optional[str] = None,
    usage_log_path: Optional[Path] = None,
) -> Tuple[int, int]:
    documents = _read_ndjson(documents_path, "document")
    passages = _read_ndjson(passages_path, "passage")
    by_id: Dict[str, Dict[str, object]] = {}
    passage_indexes: Dict[str, List[Tuple[int, int]]] = {}
    texts: List[str] = []
    for document in documents:
        document_id = document.get("id")
        if document.get("operation") != "upsert" or not isinstance(document_id, str) or not document_id or document_id in by_id:
            raise WikipediaDataError("documents must be unique canonical upsert operations")
        by_id[document_id] = document
        passage_indexes[document_id] = []
    for passage in passages:
        document_id = passage.get("document_id")
        ordinal = passage.get("ordinal")
        text = passage.get("text")
        if not isinstance(document_id, str) or document_id not in by_id or not isinstance(ordinal, int) or isinstance(ordinal, bool):
            raise WikipediaDataError("passage refers to an invalid document or ordinal")
        if not isinstance(text, str) or not text.strip():
            raise WikipediaDataError(
                "passage text must be non-empty for {} ordinal {}".format(document_id, ordinal)
            )
        expected = len(passage_indexes[document_id])
        if ordinal != expected:
            raise WikipediaDataError("passage ordinals for {} must be contiguous and ordered".format(document_id))
        passage_indexes[document_id].append((ordinal, len(texts)))
        if profile == "embeddinggemma":
            title = by_id[document_id].get("title")
            texts.append("title: {} | text: {}".format(title.strip() if isinstance(title, str) and title.strip() else "none", text))
        else:
            texts.append(text)
    if not documents or not texts or any(not indexes for indexes in passage_indexes.values()):
        raise WikipediaDataError("every input document must have at least one prepared passage")
    vectors: List[List[float]] = []
    for offset in range(0, len(texts), batch_size):
        vectors.extend(_embedding_batch(
            provider, endpoint_url, model, texts[offset:offset + batch_size], dimensions, timeout,
            api_key, usage_log_path,
        ))
    if len(vectors) != len(texts):
        raise WikipediaDataError("embedding count does not match the passage count")
    with _atomic_text_output(output_path) as output:
        for document in documents:
            document_id = str(document["id"])
            enriched = dict(document)
            enriched["vectors"] = [vectors[index] for _, index in passage_indexes[document_id]]
            _write_document(output, enriched)
    return len(documents), len(passages)


def _canonical_document(
    page_id: object,
    title: object,
    body: object,
    url: object,
    revision_id: Optional[object] = None,
) -> Optional[Dict[str, object]]:
    if not isinstance(page_id, (str, int)) or isinstance(page_id, bool):
        raise WikipediaDataError("article is missing a valid page ID")
    if not isinstance(title, str) or not title.strip():
        raise WikipediaDataError("article {} is missing a title".format(page_id))
    if not isinstance(body, str):
        raise WikipediaDataError("article {} has a non-string body".format(page_id))
    normalized_body = body.replace("\r\n", "\n").replace("\r", "\n").strip()
    if not normalized_body:
        return None
    if not isinstance(url, str) or not url.startswith(("https://", "http://")):
        raise WikipediaDataError("article {} is missing a valid URL".format(page_id))
    metadata: Dict[str, object] = {
        "language": "ja",
        "source": "wikipedia-ja",
        "wikipedia_page_id": str(page_id),
    }
    if isinstance(revision_id, int) and not isinstance(revision_id, bool):
        metadata["wikipedia_revision_id"] = revision_id
    return {
        "operation": "upsert",
        "id": "jawiki:{}".format(page_id),
        "url": url,
        "title": title.strip(),
        "body": normalized_body,
        "metadata": metadata,
    }


@contextlib.contextmanager
def _atomic_text_output(path: Path) -> Iterator[TextIO]:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp", dir=str(path.parent))
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as output:
            yield output
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary_name, str(path))
    except BaseException:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass
        raise


def _write_document(output: TextIO, document: Dict[str, object]) -> None:
    output.write(json.dumps(document, ensure_ascii=False, separators=(",", ":")))
    output.write("\n")


def _fetch_api_article(
    api_url: str,
    page_id: object,
    user_agent: str,
) -> Optional[Dict[str, object]]:
    parameters = {
        "action": "query",
        "format": "json",
        "formatversion": "2",
        "pageids": str(page_id),
        "prop": "extracts|info",
        "explaintext": "1",
        "exsectionformat": "plain",
        "inprop": "url",
        "redirects": "1",
    }
    separator = "&" if "?" in api_url else "?"
    response = _request_json(api_url + separator + urlencode(parameters), user_agent)
    query = response.get("query", {})
    pages = query.get("pages", []) if isinstance(query, dict) else []
    if not isinstance(pages, list) or len(pages) != 1 or not isinstance(pages[0], dict):
        raise WikipediaDataError("Wikimedia API article response must contain one page")
    page = pages[0]
    return _canonical_document(
        page.get("pageid"),
        page.get("title"),
        page.get("extract"),
        page.get("fullurl"),
        page.get("lastrevid"),
    )


def fetch_api_documents(
    api_url: str,
    topics: Iterable[str],
    limit: int,
    output_path: Path,
    user_agent: str,
) -> Tuple[int, int]:
    topic_list = list(topics)
    seen = set()
    written = 0
    skipped = 0
    with _atomic_text_output(output_path) as output:
        for topic_index, topic in enumerate(topic_list):
            if written >= limit:
                break
            remaining_topics = len(topic_list) - topic_index
            topic_target = written + (limit - written + remaining_topics - 1) // remaining_topics
            continuation: Dict[str, object] = {}
            while written < topic_target:
                batch_limit = min(API_SEARCH_LIMIT, topic_target - written)
                parameters: Dict[str, object] = {
                    "action": "query",
                    "format": "json",
                    "formatversion": "2",
                    "generator": "search",
                    "gsrsearch": topic,
                    "gsrnamespace": "0",
                    "gsrlimit": str(batch_limit),
                    "redirects": "1",
                }
                parameters.update(continuation)
                separator = "&" if "?" in api_url else "?"
                response = _request_json(api_url + separator + urlencode(parameters), user_agent)
                query = response.get("query", {})
                pages = query.get("pages", []) if isinstance(query, dict) else []
                if not isinstance(pages, list):
                    raise WikipediaDataError("Wikimedia API pages must be an array")
                for page in pages:
                    if written >= topic_target:
                        break
                    if not isinstance(page, dict):
                        raise WikipediaDataError("Wikimedia API page must be an object")
                    page_id = page.get("pageid")
                    key = str(page_id)
                    if key in seen:
                        skipped += 1
                        continue
                    document = _fetch_api_article(api_url, page_id, user_agent)
                    seen.add(key)
                    if document is None:
                        skipped += 1
                        continue
                    _write_document(output, document)
                    written += 1
                next_page = response.get("continue")
                if not isinstance(next_page, dict) or "gsroffset" not in next_page:
                    break
                continuation = next_page
        if written == 0:
            raise WikipediaDataError("no non-empty Wikipedia articles were returned")
    return written, skipped


def _open_text(path: Path) -> TextIO:
    if path.suffix == ".bz2":
        return bz2.open(str(path), "rt", encoding="utf-8")
    if path.suffix == ".gz":
        return gzip.open(str(path), "rt", encoding="utf-8")
    return path.open("r", encoding="utf-8")


def _input_files(path: Path) -> List[Path]:
    if path.is_file():
        return [path]
    if not path.is_dir():
        raise WikipediaDataError("WikiExtractor input does not exist: {}".format(path))
    files = [candidate for candidate in path.rglob("*") if candidate.is_file() and not candidate.name.startswith(".")]
    if not files:
        raise WikipediaDataError("WikiExtractor input directory is empty: {}".format(path))
    return sorted(files)


def convert_wikiextractor(
    input_path: Path,
    output_path: Path,
    limit: Optional[int] = None,
) -> Tuple[int, int]:
    seen = set()
    written = 0
    skipped = 0
    with _atomic_text_output(output_path) as output:
        for source in _input_files(input_path):
            try:
                with _open_text(source) as input_file:
                    for line_number, line in enumerate(input_file, 1):
                        if limit is not None and written >= limit:
                            break
                        if not line.strip():
                            continue
                        try:
                            article = json.loads(line)
                        except json.JSONDecodeError as error:
                            raise WikipediaDataError(
                                "invalid WikiExtractor JSON at {}:{}".format(source, line_number)
                            ) from error
                        if not isinstance(article, dict):
                            raise WikipediaDataError("WikiExtractor record must be an object")
                        page_id = article.get("id")
                        key = str(page_id)
                        if key in seen:
                            skipped += 1
                            continue
                        document = _canonical_document(
                            page_id,
                            article.get("title"),
                            article.get("text"),
                            article.get("url"),
                            article.get("revid"),
                        )
                        seen.add(key)
                        if document is None:
                            skipped += 1
                            continue
                        _write_document(output, document)
                        written += 1
            except (UnicodeDecodeError, OSError) as error:
                raise WikipediaDataError("cannot read {}: {}".format(source, error)) from error
            if limit is not None and written >= limit:
                break
        if written == 0:
            raise WikipediaDataError("WikiExtractor output contained no non-empty articles")
    return written, skipped


def _download_file(
    url: str,
    destination: Path,
    user_agent: str,
    publish: bool = True,
    retries: int = 4,
) -> Path:
    destination.parent.mkdir(parents=True, exist_ok=True)
    partial = destination.with_name(destination.name + ".part")
    for attempt in range(retries + 1):
        offset = partial.stat().st_size if partial.exists() else 0
        headers = {"User-Agent": user_agent}
        if offset:
            headers["Range"] = "bytes={}-".format(offset)
        request = Request(url, headers=headers)
        try:
            with urlopen(request, timeout=60) as response:
                append = offset > 0 and getattr(response, "status", None) == 206
                mode = "ab" if append else "wb"
                with partial.open(mode) as output:
                    while True:
                        block = response.read(1024 * 1024)
                        if not block:
                            break
                        output.write(block)
                    output.flush()
                    os.fsync(output.fileno())
            if publish:
                os.replace(str(partial), str(destination))
                return destination
            return partial
        except HTTPError as error:
            if error.code == 416 and offset > 0:
                partial.unlink(missing_ok=True)
            if (error.code != 429 and error.code < 500) or attempt == retries:
                raise WikipediaDataError("download failed for {}: HTTP {}".format(url, error.code)) from error
            retry_after = error.headers.get("Retry-After")
            delay = float(retry_after) if retry_after and retry_after.isdigit() else 2 ** attempt
        except (URLError, TimeoutError, OSError) as error:
            if attempt == retries:
                raise WikipediaDataError("download failed for {}: {}".format(url, error)) from error
            delay = 2 ** attempt
        time.sleep(delay)
    raise AssertionError("unreachable")


def _sha1(path: Path) -> str:
    digest = hashlib.sha1()
    with path.open("rb") as source:
        while True:
            block = source.read(1024 * 1024)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def _find_dump_checksum(checksum_text: str) -> Tuple[str, str]:
    for line in checksum_text.splitlines():
        fields = line.split()
        if len(fields) < 2:
            continue
        filename = fields[-1].lstrip("*")
        checksum = fields[0].lower()
        if DUMP_CHECKSUM_PATTERN.fullmatch(filename) and len(checksum) == 40:
            return filename, checksum
    raise WikipediaDataError("dump checksum file does not contain the combined multistream dump")


def download_dump(base_url: str, output_dir: Path, user_agent: str) -> Path:
    base_url = base_url.rstrip("/")
    checksum_path = output_dir / CHECKSUM_FILENAME
    dump_path = output_dir / DUMP_FILENAME
    _download_file(base_url + "/" + CHECKSUM_FILENAME, checksum_path, user_agent)
    source_filename, expected = _find_dump_checksum(checksum_path.read_text(encoding="ascii"))
    if dump_path.exists() and _sha1(dump_path) == expected:
        return dump_path
    partial = _download_file(
        base_url + "/" + source_filename, dump_path, user_agent, publish=False
    )
    actual = _sha1(partial)
    if actual != expected:
        partial.unlink(missing_ok=True)
        raise WikipediaDataError("dump checksum mismatch: expected {}, got {}".format(expected, actual))
    os.replace(str(partial), str(dump_path))
    return dump_path


def _positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def _argument_paths(args: argparse.Namespace) -> List[Tuple[str, Path]]:
    paths: List[Tuple[str, Path]] = []
    for name in ("config", "input", "documents", "passages", "output", "output_dir"):
        value = getattr(args, name, None)
        if isinstance(value, Path):
            paths.append((name.replace("_", " ").title(), value.expanduser().resolve()))
    return paths


def _wikipedia_error_actions(reason: str, args: argparse.Namespace) -> List[str]:
    lower = reason.lower()
    actions: List[str] = []
    config = getattr(args, "config", None)
    if any(token in lower for token in (
        "toml config", "must contain a [", "unknown key", "must be", "must use", "must not",
        "must specify",
    )):
        config_path = config.expanduser().resolve() if isinstance(config, Path) else DEFAULT_APP_CONFIG
        actions.extend((
            f"Correct the named setting in {config_path}.",
            "Compare it with examples/wikipedia-search/wikipedia-search.example.toml.",
        ))
    elif "environment variable" in lower:
        match = re.search(r"environment variable ([A-Za-z_][A-Za-z0-9_]*)", reason)
        variable = match.group(1) if match else "the variable named by authorization_token_env"
        actions.extend((
            f"Set {variable} to the embedding service token in the current shell.",
            "Keep only the environment variable name in the TOML configuration.",
        ))
    elif any(token in lower for token in ("does not exist", "input directory is empty", "cannot read")):
        input_paths = [str(path) for name, path in _argument_paths(args) if name in {
            "Input", "Documents", "Passages"
        }]
        target = ", ".join(input_paths) if input_paths else "the path named in Reason"
        actions.extend((
            f"Confirm that the input exists and is readable: {target}",
            "Run the preceding data preparation step if this file has not been generated yet.",
        ))
    elif any(token in lower for token in ("embedding", "vector", "passage")):
        actions.extend((
            "Check the document and passage paths, then verify their IDs and passage ordinals match.",
            "Check [embedding] endpoint, model, model_id, and dimensions, and confirm the service is running.",
        ))
    elif any(token in lower for token in ("http ", "request failed", "download failed", "timed out")):
        actions.extend((
            "Confirm network access and the configured URL, then retry.",
            "For Wikimedia requests, provide a descriptive --user-agent with contact information.",
        ))
    elif "checksum" in lower:
        actions.extend((
            "Retry download-dump; incomplete temporary output is not published.",
            "If the mismatch repeats, verify free disk space and that no proxy is rewriting the download.",
        ))
    elif any(token in lower for token in (
        "invalid json", "invalid utf-8", "canonical", "ordinal", "record must", "no non-empty",
        "missing a valid", "missing a title", "non-string body",
    )):
        if args.command == "convert-dump":
            actions.extend((
                "Inspect the reported WikiExtractor JSONL record and correct or regenerate that input.",
                "Keep one valid JSON object per line with id, url, title, and text fields.",
            ))
        elif args.command == "embed":
            actions.extend((
                "Regenerate documents and passages from the same source before embedding.",
                "Do not reorder passage ordinals or mix files from different preparation runs.",
            ))
        else:
            actions.append("Retry with the official Wikimedia endpoint; if it repeats, enable debug output and report the response error.")
    elif any(token in lower for token in ("permission denied", "read-only", "cannot write", "cannot create")):
        actions.append("Check the reported output path's parent directory and its write permissions.")

    script_path = Path(__file__).resolve()
    try:
        script_name = str(script_path.relative_to(Path.cwd()))
    except ValueError:
        script_name = str(script_path)
    help_command = shlex.join((sys.executable, script_name, args.command, "--help"))
    actions.append(f"Run `{help_command}` to review this command's inputs.")
    return actions


def _format_wikipedia_error(
    error: BaseException, args: argparse.Namespace, unexpected: bool = False
) -> str:
    reason = str(error) or error.__class__.__name__
    lines = [
        "wikipedia-data: error: {!r} command failed".format(args.command),
        "Reason: {}".format(reason),
    ]
    lines.extend("{}: {}".format(name, path) for name, path in _argument_paths(args))
    lines.append("How to fix:")
    for index, action in enumerate(_wikipedia_error_actions(reason, args), 1):
        lines.append("  {}. {}".format(index, action))
    if unexpected:
        lines.append("  Debug: re-run with YAPPOD_EXAMPLE_DEBUG=1 to include the Python traceback.")
    return "\n".join(lines)


def build_parser() -> argparse.ArgumentParser:
    parser = FriendlyArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(
        dest="command", required=True, parser_class=FriendlyArgumentParser
    )

    api = subparsers.add_parser("fetch-api", help="fetch a small topic-balanced API sample")
    api.add_argument("--api-url", default=DEFAULT_API_URL)
    api.add_argument("--topic", action="append", dest="topics")
    api.add_argument("--limit", type=_positive_int, default=1000)
    api.add_argument("--output", type=Path, required=True)
    api.add_argument("--user-agent", default=DEFAULT_USER_AGENT)

    download = subparsers.add_parser("download-dump", help="download and verify the official full XML dump")
    download.add_argument("--base-url", default=DEFAULT_DUMP_BASE_URL)
    download.add_argument("--output-dir", type=Path, required=True)
    download.add_argument("--user-agent", default=DEFAULT_USER_AGENT)

    convert = subparsers.add_parser("convert-dump", help="convert WikiExtractor JSON output")
    convert.add_argument("--input", type=Path, required=True)
    convert.add_argument("--output", type=Path, required=True)
    convert.add_argument("--limit", type=_positive_int)

    embed = subparsers.add_parser("embed", help="attach LM Studio, Ollama, or OpenAI embeddings to canonical documents")
    embed.add_argument("--documents", type=Path, required=True)
    embed.add_argument("--passages", type=Path, required=True)
    embed.add_argument("--output", type=Path, required=True)
    embed.add_argument("--config", type=Path, default=DEFAULT_APP_CONFIG)
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.command == "fetch-api":
            written, skipped = fetch_api_documents(
                args.api_url, args.topics or DEFAULT_TOPICS, args.limit, args.output, args.user_agent
            )
            result = {"output": str(args.output), "written": written, "skipped": skipped}
        elif args.command == "download-dump":
            path = download_dump(args.base_url, args.output_dir, args.user_agent)
            result = {"output": str(path), "sha1": _sha1(path)}
        elif args.command == "convert-dump":
            written, skipped = convert_wikiextractor(args.input, args.output, args.limit)
            result = {"output": str(args.output), "written": written, "skipped": skipped}
        else:
            settings = load_embedding_settings(args.config)
            documents, passages = embed_documents(
                args.documents, args.passages, args.output, settings.provider, settings.endpoint_url,
                settings.model, settings.dimensions, settings.batch_size, settings.timeout,
                settings.profile, settings.authorization_token, settings.usage_log_path,
            )
            result = {"output": str(args.output), "documents": documents, "passages": passages}
    except WikipediaDataError as error:
        print(_format_wikipedia_error(error, args), file=sys.stderr)
        return 1
    except Exception as error:
        if os.environ.get("YAPPOD_EXAMPLE_DEBUG") == "1":
            raise
        print(_format_wikipedia_error(error, args, unexpected=True), file=sys.stderr)
        return 1
    print(json.dumps(result, ensure_ascii=False, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
