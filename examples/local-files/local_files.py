#!/usr/bin/env python3
"""Convert local files into shards and build lexical, RAG, or hybrid indexes."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import fnmatch
import hashlib
from html.parser import HTMLParser
import ipaddress
import json
import math
import mimetypes
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass
from typing import Any, Dict, Iterator, List, Mapping, Optional, Sequence, Tuple
import unicodedata
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import Request, urlopen
import xml.etree.ElementTree as ElementTree

try:
    import tomllib
except ModuleNotFoundError:  # Python 3.9-3.10
    try:
        import tomli as tomllib  # type: ignore[no-redef]
    except ModuleNotFoundError:
        print(
            "local-files: TOML parser is unavailable; run "
            "python3 -m pip install -r examples/local-files/requirements-core.txt",
            file=sys.stderr,
        )
        raise SystemExit(2)


SCHEMA_VERSION = 1
DEFAULT_SHARD_BYTES = 64 * 1024 * 1024
DEFAULT_BODY_BYTES = 1_000_000
YAPPOD_BODY_LIMIT = 1024 * 1024
DEFAULT_SCAN_BYTES = 1024 * 1024
DEFAULT_EXTRACTED_BYTES = 64 * 1024 * 1024
COLLECTION_PATTERN = re.compile(r"^[A-Za-z0-9._-]{1,32}$")
PART_ORDINAL_DIGITS = 10
BUILD_STATE_FILENAME = "local-files-build.json"

TEXT_SUFFIXES = {
    ".asm", ".bash", ".c", ".cc", ".cfg", ".clj", ".cmake", ".conf",
    ".cpp", ".cs", ".css", ".csv", ".dart", ".el", ".erl", ".ex",
    ".fish", ".go", ".h", ".hh", ".hpp", ".hs", ".ini", ".java",
    ".js", ".jsx", ".kt", ".lua", ".m", ".md", ".mk", ".mm", ".php",
    ".pl", ".pm", ".properties", ".proto", ".py", ".r", ".rb", ".rs",
    ".rst", ".scala", ".sh", ".sql", ".swift", ".tex", ".text", ".toml",
    ".ts", ".tsx", ".txt", ".vue", ".xml", ".yaml", ".yml", ".zsh",
}
JSON_SUFFIXES = {".json"}
JSON_LINES_SUFFIXES = {".jsonl", ".ndjson"}
HTML_SUFFIXES = {".htm", ".html", ".xhtml"}
XML_SUFFIXES = {".xml"}
MARKITDOWN_SUFFIXES = {".pdf", ".docx", ".xlsx", ".xls", ".pptx"}
TIKA_SUFFIXES = {".doc", ".ppt", ".key", ".pages", ".numbers"}


class LocalFilesError(Exception):
    """Fatal configuration or pipeline error."""


class FileFailure(Exception):
    """A recoverable failure for one source file."""

    def __init__(self, code: str, message: str, extractor: Optional[str] = None):
        super().__init__(message)
        self.code = code
        self.message = message
        self.extractor = extractor


@dataclass(frozen=True)
class FormatterRule:
    name: str
    basename_glob: Tuple[str, ...]
    path_glob: Tuple[str, ...]
    path_regex: Tuple[re.Pattern[str], ...]
    content_regex: Tuple[re.Pattern[str], ...]
    command: Tuple[str, ...]
    timeout_ms: int
    max_stdout_bytes: int


@dataclass(frozen=True)
class EmbeddingConfig:
    provider: str
    base_url: Optional[str]
    endpoint_url: str
    model: str
    model_id: str
    dimensions: int
    batch_size: int
    timeout_ms: int
    prompt_profile: str
    authorization_token: Optional[str]
    usage_log_path: Optional[Path]


@dataclass(frozen=True)
class Settings:
    config_path: Path
    config_dir: Path
    collection_id: str
    root: Path
    output_dir: Path
    include: Tuple[str, ...]
    exclude: Tuple[str, ...]
    follow_symlinks: bool
    shard_max_bytes: int
    body_max_bytes: int
    max_extracted_bytes: int
    enable_plugins: bool
    tika_command: Tuple[str, ...]
    tika_timeout_ms: int
    formatters_enabled: bool
    content_match_enabled: bool
    content_scan_bytes: int
    formatter_rules: Tuple[FormatterRule, ...]
    passage_dir: Optional[Path]
    vector_dir: Optional[Path]
    embedding: Optional[EmbeddingConfig]
    yappo_makeindex: Optional[Path]
    index_config: Optional[Path]
    hybrid_index_config: Optional[Path]
    index_dir: Optional[Path]
    usage_log_path: Optional[Path]
    config_fingerprint: str


class _HTMLToMarkdown(HTMLParser):
    BLOCK_TAGS = {
        "address", "article", "aside", "blockquote", "br", "div", "dl", "dt",
        "dd", "footer", "form", "header", "hr", "main", "nav", "ol", "p",
        "pre", "section", "table", "tr", "ul",
    }

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.parts: List[str] = []
        self.skip_depth = 0

    def handle_starttag(self, tag: str, attrs: List[Tuple[str, Optional[str]]]) -> None:
        del attrs
        tag = tag.lower()
        if tag in {"script", "style", "noscript"}:
            self.skip_depth += 1
            return
        if self.skip_depth:
            return
        if tag in self.BLOCK_TAGS:
            self.parts.append("\n")
        if tag in {"h1", "h2", "h3", "h4", "h5", "h6"}:
            self.parts.append("#" * int(tag[1]) + " ")
        elif tag == "li":
            self.parts.append("- ")

    def handle_endtag(self, tag: str) -> None:
        tag = tag.lower()
        if tag in {"script", "style", "noscript"}:
            if self.skip_depth:
                self.skip_depth -= 1
            return
        if not self.skip_depth and (tag in self.BLOCK_TAGS or tag.startswith("h")):
            self.parts.append("\n")

    def handle_data(self, data: str) -> None:
        if not self.skip_depth:
            self.parts.append(data)

    def text(self) -> str:
        lines = [" ".join(line.split()) for line in "".join(self.parts).splitlines()]
        compact: List[str] = []
        for line in lines:
            if line:
                compact.append(line)
            elif compact and compact[-1] != "":
                compact.append("")
        return "\n".join(compact).strip()


def _expect_table(value: Any, name: str) -> Mapping[str, Any]:
    if value is None:
        return {}
    if not isinstance(value, dict):
        raise LocalFilesError(f"{name} must be a TOML table")
    return value


def _string_list(value: Any, name: str, default: Sequence[str] = ()) -> Tuple[str, ...]:
    if value is None:
        return tuple(default)
    if not isinstance(value, list) or any(not isinstance(item, str) or not item for item in value):
        raise LocalFilesError(f"{name} must be an array of non-empty strings")
    return tuple(value)


def _positive_int(value: Any, name: str, default: int) -> int:
    if value is None:
        return default
    if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
        raise LocalFilesError(f"{name} must be a positive integer")
    return value


def _required_positive_int(value: Any, name: str) -> int:
    if value is None:
        raise LocalFilesError(f"{name} is required")
    return _positive_int(value, name, 1)


def _bool(value: Any, name: str, default: bool) -> bool:
    if value is None:
        return default
    if not isinstance(value, bool):
        raise LocalFilesError(f"{name} must be true or false")
    return value


def _nonempty_string(value: Any, name: str, required: bool = True) -> Optional[str]:
    if value is None and not required:
        return None
    if not isinstance(value, str) or not value.strip():
        raise LocalFilesError(f"{name} must be a non-empty string")
    return value.strip()


def _resolve(config_dir: Path, value: Any, name: str) -> Path:
    if not isinstance(value, str) or not value:
        raise LocalFilesError(f"{name} must be a non-empty path string")
    path = Path(value).expanduser()
    if not path.is_absolute():
        path = config_dir / path
    return path.resolve(strict=False)


def _optional_resolve(config_dir: Path, value: Any, name: str) -> Optional[Path]:
    if value is None:
        return None
    return _resolve(config_dir, value, name)


_HTTP_IPV4_NETWORKS = tuple(
    ipaddress.ip_network(value)
    for value in ("127.0.0.0/8", "10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16")
)
_HTTP_IPV6_NETWORKS = tuple(
    ipaddress.ip_network(value) for value in ("::1/128", "fc00::/7")
)


def _http_host_allowed(host: Optional[str]) -> bool:
    if host is None:
        return False
    normalized = host.rstrip(".").lower()
    if normalized == "localhost":
        return True
    try:
        address = ipaddress.ip_address(normalized)
    except ValueError:
        return False
    networks = _HTTP_IPV4_NETWORKS if address.version == 4 else _HTTP_IPV6_NETWORKS
    return any(address in network for network in networks)


def _service_url(value: Any, name: str, *, base: bool = False) -> str:
    result = _nonempty_string(value, name)
    assert result is not None
    parsed = urlsplit(result)
    if parsed.scheme not in {"http", "https"} or not parsed.netloc or parsed.hostname is None:
        raise LocalFilesError(f"{name} must be an http or https URL")
    if parsed.scheme == "http" and not _http_host_allowed(parsed.hostname):
        raise LocalFilesError(f"{name} must use https outside localhost, loopback, or private networks")
    if base and (parsed.query or parsed.fragment):
        raise LocalFilesError(f"{name} must not contain a query or fragment")
    return result.rstrip("/") if base else result


def _authorization_token_from_env(table: Mapping[str, Any], name: str) -> Optional[str]:
    if "authorization_token" in table:
        raise LocalFilesError(
            f"{name}.authorization_token is not supported; use {name}.authorization_token_env"
        )
    environment_name = _nonempty_string(
        table.get("authorization_token_env"),
        f"{name}.authorization_token_env",
        required=False,
    )
    if environment_name is None:
        return None
    if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", environment_name) is None:
        raise LocalFilesError(f"{name}.authorization_token_env must be an environment variable name")
    token = os.environ.get(environment_name)
    if token is None or not token.strip():
        raise LocalFilesError(f"environment variable {environment_name} is not set or empty")
    return token.strip()


def _append_usage_log(
    path: Optional[Path],
    *,
    source: str,
    service: str,
    operation: str,
    provider: str,
    model: str,
    usage: Any,
) -> None:
    if path is None:
        return
    record = {
        "timestamp": datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z"),
        "source": source,
        "service": service,
        "operation": operation,
        "provider": provider,
        "model": model,
        "usage": usage if isinstance(usage, dict) else None,
    }
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("a", encoding="utf-8") as output:
            output.write(json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n")
    except OSError as error:
        print(f"local-files: warning: cannot append usage log {path}: {error}", file=sys.stderr)


def _compile_regex_list(value: Any, name: str) -> Tuple[re.Pattern[str], ...]:
    patterns = _string_list(value, name)
    compiled: List[re.Pattern[str]] = []
    for pattern in patterns:
        try:
            compiled.append(re.compile(pattern))
        except re.error as error:
            raise LocalFilesError(f"invalid {name} pattern {pattern!r}: {error}") from error
    return tuple(compiled)


def load_settings(config_path: Path, content_match_override: Optional[bool] = None) -> Settings:
    config_path = config_path.expanduser().resolve()
    try:
        raw = config_path.read_bytes()
        data = tomllib.loads(raw.decode("utf-8"))
    except (OSError, UnicodeDecodeError, tomllib.TOMLDecodeError) as error:
        raise LocalFilesError(f"cannot load config {config_path}: {error}") from error
    if not isinstance(data, dict):
        raise LocalFilesError("config root must be a TOML table")
    if data.get("schema_version", SCHEMA_VERSION) != SCHEMA_VERSION:
        raise LocalFilesError(f"schema_version must be {SCHEMA_VERSION}")
    collection_id = data.get("collection_id")
    if not isinstance(collection_id, str) or not COLLECTION_PATTERN.fullmatch(collection_id):
        raise LocalFilesError("collection_id must match [A-Za-z0-9._-]{1,32}")

    config_dir = config_path.parent
    input_table = _expect_table(data.get("input"), "input")
    output_table = _expect_table(data.get("output"), "output")
    extract_table = _expect_table(data.get("extract"), "extract")
    formatters_table = _expect_table(data.get("formatters"), "formatters")
    prepare_table = _expect_table(data.get("prepare"), "prepare")
    embedding_table = _expect_table(data.get("embedding"), "embedding")
    build_table = _expect_table(data.get("build"), "build")
    usage_log_table = _expect_table(data.get("usage_log"), "usage_log")
    root = _resolve(config_dir, input_table.get("root"), "input.root")
    output_dir = _resolve(config_dir, output_table.get("directory"), "output.directory")
    shard_max_bytes = _positive_int(
        output_table.get("shard_max_bytes"), "output.shard_max_bytes", DEFAULT_SHARD_BYTES
    )
    body_max_bytes = _positive_int(
        output_table.get("body_max_bytes"), "output.body_max_bytes", DEFAULT_BODY_BYTES
    )
    if body_max_bytes > YAPPOD_BODY_LIMIT:
        raise LocalFilesError(f"output.body_max_bytes cannot exceed {YAPPOD_BODY_LIMIT}")

    rules_value = formatters_table.get("rules", [])
    if not isinstance(rules_value, list):
        raise LocalFilesError("formatters.rules must be an array of tables")
    rules: List[FormatterRule] = []
    names = set()
    for index, item in enumerate(rules_value):
        if not isinstance(item, dict):
            raise LocalFilesError(f"formatters.rules[{index}] must be a table")
        name = item.get("name")
        if not isinstance(name, str) or not name or name in names:
            raise LocalFilesError(f"formatters.rules[{index}].name must be non-empty and unique")
        names.add(name)
        command = _string_list(item.get("command"), f"formatters.rules[{index}].command")
        if not command or sum(part.count("{path}") for part in command) != 1:
            raise LocalFilesError(f"formatter {name!r} command must contain {{path}} exactly once")
        rules.append(
            FormatterRule(
                name=name,
                basename_glob=_string_list(item.get("basename_glob"), f"formatter {name}.basename_glob"),
                path_glob=_string_list(item.get("path_glob"), f"formatter {name}.path_glob"),
                path_regex=_compile_regex_list(item.get("path_regex"), f"formatter {name}.path_regex"),
                content_regex=_compile_regex_list(item.get("content_regex"), f"formatter {name}.content_regex"),
                command=command,
                timeout_ms=_positive_int(item.get("timeout_ms"), f"formatter {name}.timeout_ms", 30_000),
                max_stdout_bytes=_positive_int(
                    item.get("max_stdout_bytes"), f"formatter {name}.max_stdout_bytes", DEFAULT_EXTRACTED_BYTES
                ),
            )
        )

    tika_command = _string_list(extract_table.get("tika_command"), "extract.tika_command")
    if tika_command and sum(part.count("{path}") for part in tika_command) != 1:
        raise LocalFilesError("extract.tika_command must contain {path} exactly once")
    content_match_enabled = _bool(
        formatters_table.get("content_match_enabled"), "formatters.content_match_enabled", False
    )
    if content_match_override is not None:
        content_match_enabled = content_match_override

    embedding: Optional[EmbeddingConfig] = None
    usage_log_path = _optional_resolve(
        config_dir, usage_log_table.get("path"), "usage_log.path"
    )
    if usage_log_table and usage_log_path is None:
        raise LocalFilesError("usage_log.path is required")
    if embedding_table:
        provider = _nonempty_string(embedding_table.get("provider"), "embedding.provider")
        if provider not in {"lmstudio", "ollama", "openai"}:
            raise LocalFilesError("embedding.provider must be lmstudio, ollama, or openai")
        base_value = embedding_table.get("base_url")
        endpoint_value = embedding_table.get("endpoint_url")
        if (base_value is None) == (endpoint_value is None):
            raise LocalFilesError("embedding must specify exactly one of base_url or endpoint_url")
        base_url = _service_url(base_value, "embedding.base_url", base=True) if base_value is not None else None
        if endpoint_value is not None:
            endpoint_url = _service_url(endpoint_value, "embedding.endpoint_url")
        else:
            assert base_url is not None
            endpoint_url = base_url + ("/api/embed" if provider == "ollama" else "/embeddings")
        prompt_profile = _nonempty_string(
            embedding_table.get("prompt_profile", "plain"), "embedding.prompt_profile"
        )
        if prompt_profile not in {"plain", "embeddinggemma"}:
            raise LocalFilesError("embedding.prompt_profile must be plain or embeddinggemma")
        embedding = EmbeddingConfig(
            provider=provider,
            base_url=base_url,
            endpoint_url=endpoint_url,
            model=_nonempty_string(embedding_table.get("model"), "embedding.model") or "",
            model_id=_nonempty_string(embedding_table.get("model_id"), "embedding.model_id") or "",
            dimensions=_required_positive_int(
                embedding_table.get("dimensions"), "embedding.dimensions"
            ),
            batch_size=_positive_int(
                embedding_table.get("batch_size"), "embedding.batch_size", 16
            ),
            timeout_ms=_positive_int(
                embedding_table.get("timeout_ms"), "embedding.timeout_ms", 60_000
            ),
            prompt_profile=prompt_profile,
            authorization_token=_authorization_token_from_env(embedding_table, "embedding"),
            usage_log_path=usage_log_path,
        )
        if embedding.dimensions > 65536:
            raise LocalFilesError("embedding.dimensions cannot exceed 65536")
        if embedding.batch_size > 1024:
            raise LocalFilesError("embedding.batch_size cannot exceed 1024")
    fingerprint_input = raw
    if content_match_override is not None:
        fingerprint_input += (
            b"\0cli.content_match=true" if content_match_override else b"\0cli.content_match=false"
        )
    passage_dir = _optional_resolve(
        config_dir, prepare_table.get("directory"), "prepare.directory"
    )
    vector_dir = _optional_resolve(
        config_dir, embedding_table.get("directory"), "embedding.directory"
    )
    index_dir = _optional_resolve(
        config_dir, build_table.get("index_directory"), "build.index_directory"
    )
    stage_directories = {
        "output.directory": output_dir,
        "prepare.directory": passage_dir,
        "embedding.directory": vector_dir,
        "build.index_directory": index_dir,
    }
    configured_directories = [
        (name, path) for name, path in stage_directories.items() if path is not None
    ]
    for left_index, (left_name, left_path) in enumerate(configured_directories):
        for right_name, right_path in configured_directories[left_index + 1:]:
            try:
                left_path.relative_to(right_path)
                overlaps = True
            except ValueError:
                try:
                    right_path.relative_to(left_path)
                    overlaps = True
                except ValueError:
                    overlaps = False
            if overlaps:
                raise LocalFilesError(
                    f"{left_name} and {right_name} must be separate, non-nested directories"
                )
    return Settings(
        config_path=config_path,
        config_dir=config_dir,
        collection_id=collection_id,
        root=root,
        output_dir=output_dir,
        include=_string_list(input_table.get("include"), "input.include", ("*", "**/*")),
        exclude=_string_list(
            input_table.get("exclude"), "input.exclude", (".git/**", ".venv/**", "__pycache__/**")
        ),
        follow_symlinks=_bool(input_table.get("follow_symlinks"), "input.follow_symlinks", False),
        shard_max_bytes=shard_max_bytes,
        body_max_bytes=body_max_bytes,
        max_extracted_bytes=_positive_int(
            extract_table.get("max_extracted_bytes"), "extract.max_extracted_bytes", DEFAULT_EXTRACTED_BYTES
        ),
        enable_plugins=_bool(extract_table.get("enable_plugins"), "extract.enable_plugins", False),
        tika_command=tika_command,
        tika_timeout_ms=_positive_int(
            extract_table.get("tika_timeout_ms"), "extract.tika_timeout_ms", 30_000
        ),
        formatters_enabled=_bool(formatters_table.get("enabled"), "formatters.enabled", True),
        content_match_enabled=content_match_enabled,
        content_scan_bytes=_positive_int(
            formatters_table.get("content_scan_bytes"), "formatters.content_scan_bytes", DEFAULT_SCAN_BYTES
        ),
        formatter_rules=tuple(rules),
        passage_dir=passage_dir,
        vector_dir=vector_dir,
        embedding=embedding,
        yappo_makeindex=_optional_resolve(
            config_dir, build_table.get("yappo_makeindex"), "build.yappo_makeindex"
        ),
        index_config=_optional_resolve(
            config_dir, build_table.get("index_config"), "build.index_config"
        ),
        hybrid_index_config=_optional_resolve(
            config_dir, build_table.get("hybrid_index_config"), "build.hybrid_index_config"
        ),
        index_dir=index_dir,
        usage_log_path=usage_log_path,
        config_fingerprint=hashlib.sha256(fingerprint_input).hexdigest(),
    )


def normalize_relative_path(path: Path, root: Path) -> str:
    relative = path.relative_to(root).as_posix()
    return unicodedata.normalize("NFC", relative)


def document_id(collection_id: str, relative_path: str, part: int) -> str:
    if part < 0 or part >= 10**PART_ORDINAL_DIGITS:
        raise LocalFilesError("document part ordinal is out of range")
    digest = hashlib.sha256(
        collection_id.encode("ascii") + b"\0" + relative_path.encode("utf-8")
    ).hexdigest()
    return f"lf:v1:{collection_id}:{digest}:p{part:0{PART_ORDINAL_DIGITS}d}"


def truncate_utf8(text: str, limit: int) -> str:
    encoded = text.encode("utf-8")
    if len(encoded) <= limit:
        return text
    cut = limit
    while cut > 0 and encoded[cut] & 0xC0 == 0x80:
        cut -= 1
    return encoded[:cut].decode("utf-8")


def split_body(text: str, max_bytes: int) -> List[str]:
    data = text.encode("utf-8")
    if not data:
        return []
    parts: List[str] = []
    offset = 0
    while len(data) - offset > max_bytes:
        cut = offset + max_bytes
        while cut > offset and data[cut] & 0xC0 == 0x80:
            cut -= 1
        window = data[offset:cut]
        minimum = max_bytes // 2
        paragraph = window.rfind(b"\n\n", minimum)
        newline = window.rfind(b"\n", minimum)
        if paragraph >= 0:
            cut = offset + paragraph + 2
        elif newline >= 0:
            cut = offset + newline + 1
        if cut <= offset:
            raise LocalFilesError("cannot split UTF-8 body within configured limit")
        parts.append(data[offset:cut].decode("utf-8"))
        offset = cut
    parts.append(data[offset:].decode("utf-8"))
    return [part for part in parts if part]


def _glob_matches(value: str, patterns: Sequence[str]) -> bool:
    pure = PurePosixPath(value)
    return any(fnmatch.fnmatchcase(value, pattern) or pure.match(pattern) for pattern in patterns)


def _selected(relative_path: str, settings: Settings) -> bool:
    if settings.include and not _glob_matches(relative_path, settings.include):
        return False
    return not (settings.exclude and _glob_matches(relative_path, settings.exclude))


def _walk_sorted(
    root: Path,
    excluded_roots: Sequence[Path],
    exclude_patterns: Sequence[str],
    follow_symlinks: bool,
) -> Iterator[Path]:
    excluded = {path.resolve(strict=False) for path in excluded_roots}
    visited_directories = set()

    def walk(directory: Path) -> Iterator[Path]:
        if follow_symlinks:
            try:
                info = directory.stat()
            except OSError as error:
                raise LocalFilesError(f"cannot stat directory {directory}: {error}") from error
            identity = (info.st_dev, info.st_ino)
            if identity in visited_directories:
                return
            visited_directories.add(identity)
        try:
            entries = sorted(os.scandir(directory), key=lambda item: unicodedata.normalize("NFC", item.name))
        except OSError as error:
            raise LocalFilesError(f"cannot scan directory {directory}: {error}") from error
        for entry in entries:
            path = Path(entry.path)
            resolved = path.resolve(strict=False)
            if resolved in excluded:
                continue
            try:
                if entry.is_symlink() and not follow_symlinks:
                    continue
                if entry.is_dir(follow_symlinks=follow_symlinks):
                    relative = normalize_relative_path(path, root)
                    probe = f"{relative}/__local_files_probe__"
                    if exclude_patterns and (
                        _glob_matches(relative, exclude_patterns)
                        or _glob_matches(probe, exclude_patterns)
                    ):
                        continue
                    yield from walk(path)
                elif entry.is_file(follow_symlinks=follow_symlinks):
                    yield path
            except OSError:
                yield path

    yield from walk(root)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as error:
        raise FileFailure("io_error", str(error)) from error
    return digest.hexdigest()


def _read_limited(path: Path, limit: int) -> bytes:
    try:
        with path.open("rb") as source:
            data = source.read(limit + 1)
    except OSError as error:
        raise FileFailure("io_error", str(error)) from error
    if len(data) > limit:
        raise FileFailure("output_limit", f"input exceeds configured extraction limit of {limit} bytes")
    return data


def _decode_text(data: bytes) -> str:
    if b"\0" in data[:8192]:
        raise UnicodeDecodeError("text", data, 0, 1, "NUL byte detected")
    encodings = []
    if data.startswith((b"\xff\xfe", b"\xfe\xff")):
        encodings.append("utf-16")
    encodings.extend(("utf-8-sig", "utf-8"))
    for encoding in encodings:
        try:
            return data.decode(encoding).replace("\r\n", "\n").replace("\r", "\n")
        except UnicodeDecodeError:
            pass
    try:
        from charset_normalizer import from_bytes

        best = from_bytes(data).best()
        if best is not None:
            return str(best).replace("\r\n", "\n").replace("\r", "\n")
    except (ImportError, UnicodeError):
        pass
    raise UnicodeDecodeError("text", data, 0, min(1, len(data)), "unsupported text encoding")


def _command_text(
    command: Sequence[str], path: Path, cwd: Path, timeout_ms: int, max_stdout_bytes: int, extractor: str
) -> str:
    argv = [part.replace("{path}", str(path.resolve())) for part in command]
    with tempfile.TemporaryFile() as stdout_file, tempfile.TemporaryFile() as stderr_file:
        try:
            process = subprocess.Popen(
                argv, cwd=cwd, stdin=subprocess.DEVNULL, stdout=stdout_file, stderr=stderr_file, shell=False
            )
        except OSError as error:
            raise FileFailure("extractor_unavailable", str(error), extractor) from error
        deadline = time.monotonic() + timeout_ms / 1000.0
        while True:
            if os.fstat(stdout_file.fileno()).st_size > max_stdout_bytes:
                process.kill()
                process.wait()
                raise FileFailure(
                    "output_limit", f"extractor output exceeds {max_stdout_bytes} bytes", extractor
                )
            return_code = process.poll()
            if return_code is not None:
                break
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                process.kill()
                process.wait()
                raise FileFailure("timeout", f"extractor exceeded {timeout_ms} ms", extractor)
            time.sleep(min(0.01, remaining))
        stdout_file.seek(0, os.SEEK_END)
        stdout_bytes = stdout_file.tell()
        if stdout_bytes > max_stdout_bytes:
            raise FileFailure(
                "output_limit", f"extractor output exceeds {max_stdout_bytes} bytes", extractor
            )
        stderr_file.seek(0)
        stderr = stderr_file.read(4096).decode("utf-8", errors="replace").strip()
        if return_code != 0:
            detail = f": {stderr}" if stderr else ""
            raise FileFailure("extractor_failed", f"extractor exited with {return_code}{detail}", extractor)
        stdout_file.seek(0)
        data = stdout_file.read()
    try:
        text = data.decode("utf-8").replace("\r\n", "\n").replace("\r", "\n")
    except UnicodeDecodeError as error:
        raise FileFailure("invalid_utf8", "extractor stdout is not UTF-8", extractor) from error
    if not text.strip():
        raise FileFailure("no_text", "extractor returned no searchable text", extractor)
    return text


def _rule_path_matches(rule: FormatterRule, relative_path: str, basename: str) -> bool:
    groups: List[bool] = []
    if rule.basename_glob:
        groups.append(_glob_matches(basename, rule.basename_glob))
    if rule.path_glob:
        groups.append(_glob_matches(relative_path, rule.path_glob))
    if rule.path_regex:
        groups.append(any(pattern.search(relative_path) for pattern in rule.path_regex))
    return all(groups) if groups else True


def _matching_formatter(path: Path, relative_path: str, settings: Settings) -> Optional[FormatterRule]:
    if not settings.formatters_enabled:
        return None
    for rule in settings.formatter_rules:
        if not _rule_path_matches(rule, relative_path, path.name):
            continue
        if rule.content_regex:
            if not settings.content_match_enabled:
                continue
            try:
                with path.open("rb") as source:
                    prefix = source.read(settings.content_scan_bytes)
                content = _decode_text(prefix)
            except (OSError, UnicodeDecodeError):
                continue
            if not any(pattern.search(content) for pattern in rule.content_regex):
                continue
        return rule
    return None


def _extract_json(path: Path, settings: Settings) -> str:
    data = _read_limited(path, settings.max_extracted_bytes)
    try:
        value = json.loads(_decode_text(data))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise FileFailure("invalid_format", f"invalid JSON: {error}", "json") from error
    return json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n"


def _extract_json_lines(path: Path, settings: Settings) -> str:
    data = _read_limited(path, settings.max_extracted_bytes)
    try:
        lines = _decode_text(data).splitlines()
        values = [json.loads(line) for line in lines if line.strip()]
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise FileFailure("invalid_format", f"invalid JSON Lines: {error}", "json-lines") from error
    if not values:
        raise FileFailure("no_text", "JSON Lines file contains no records", "json-lines")
    return "\n".join(json.dumps(value, ensure_ascii=False, sort_keys=True) for value in values) + "\n"


def _extract_html(path: Path, settings: Settings) -> str:
    data = _read_limited(path, settings.max_extracted_bytes)
    try:
        text = _decode_text(data)
    except UnicodeDecodeError as error:
        raise FileFailure("invalid_utf8", "HTML is not decodable text", "html") from error
    parser = _HTMLToMarkdown()
    try:
        parser.feed(text)
        parser.close()
    except Exception as error:
        raise FileFailure("invalid_format", f"invalid HTML: {error}", "html") from error
    return parser.text()


def _extract_xml(path: Path, settings: Settings) -> str:
    data = _read_limited(path, settings.max_extracted_bytes)
    if b"<!DOCTYPE" in data.upper():
        raise FileFailure("invalid_format", "DOCTYPE is not accepted for local XML extraction", "xml")
    try:
        try:
            from defusedxml import ElementTree as SafeElementTree

            root = SafeElementTree.fromstring(data)
        except ImportError:
            root = ElementTree.fromstring(data)
    except Exception as error:
        raise FileFailure("invalid_format", f"invalid XML: {error}", "xml") from error
    text = "\n".join(piece.strip() for piece in root.itertext() if piece.strip())
    return text


def _extract_markitdown(path: Path, settings: Settings) -> str:
    try:
        from markitdown import MarkItDown
    except ImportError as error:
        raise FileFailure(
            "extractor_unavailable", "MarkItDown is not installed; see examples/local-files/README.md", "markitdown"
        ) from error
    try:
        result = MarkItDown(enable_plugins=settings.enable_plugins).convert(str(path))
        text = getattr(result, "markdown", getattr(result, "text_content", ""))
    except Exception as error:
        raise FileFailure("extractor_failed", str(error), "markitdown") from error
    if not isinstance(text, str) or not text.strip():
        raise FileFailure("no_text", "MarkItDown returned no searchable text", "markitdown")
    if len(text.encode("utf-8")) > settings.max_extracted_bytes:
        raise FileFailure(
            "output_limit", f"MarkItDown output exceeds {settings.max_extracted_bytes} bytes", "markitdown"
        )
    return text


def extract_file(path: Path, relative_path: str, settings: Settings) -> Tuple[str, str]:
    rule = _matching_formatter(path, relative_path, settings)
    if rule is not None:
        name = f"formatter:{rule.name}"
        return (
            _command_text(
                rule.command, path, settings.config_dir, rule.timeout_ms, rule.max_stdout_bytes, name
            ),
            name,
        )
    suffix = path.suffix.lower()
    if suffix in JSON_SUFFIXES:
        return _extract_json(path, settings), "json"
    if suffix in JSON_LINES_SUFFIXES:
        return _extract_json_lines(path, settings), "json-lines"
    if suffix in HTML_SUFFIXES:
        return _extract_html(path, settings), "html"
    if suffix in XML_SUFFIXES:
        return _extract_xml(path, settings), "xml"
    if suffix in TIKA_SUFFIXES:
        if not settings.tika_command:
            raise FileFailure("extractor_unavailable", "Apache Tika command is not configured", "tika")
        return (
            _command_text(
                settings.tika_command,
                path,
                settings.config_dir,
                settings.tika_timeout_ms,
                settings.max_extracted_bytes,
                "tika",
            ),
            "tika",
        )
    if suffix in MARKITDOWN_SUFFIXES:
        try:
            return _extract_markitdown(path, settings), "markitdown"
        except FileFailure:
            if settings.tika_command:
                return (
                    _command_text(
                        settings.tika_command,
                        path,
                        settings.config_dir,
                        settings.tika_timeout_ms,
                        settings.max_extracted_bytes,
                        "tika",
                    ),
                    "tika",
                )
            raise
    guessed_mime = mimetypes.guess_type(path.name)[0] or ""
    if suffix in TEXT_SUFFIXES or guessed_mime.startswith("text/") or path.name in {"Dockerfile", "Makefile"}:
        try:
            return _decode_text(_read_limited(path, settings.max_extracted_bytes)), "text"
        except UnicodeDecodeError as error:
            raise FileFailure("invalid_utf8", "text file could not be decoded", "text") from error
    if settings.enable_plugins:
        return _extract_markitdown(path, settings), "markitdown-plugin"
    try:
        data = _read_limited(path, settings.max_extracted_bytes)
        return _decode_text(data), "text-detected"
    except UnicodeDecodeError as error:
        raise FileFailure("unsupported_format", "no built-in extractor accepted this file") from error


class ShardWriter:
    def __init__(self, directory: Path, prefix: str, max_bytes: int):
        self.directory = directory
        self.prefix = prefix
        self.max_bytes = max_bytes
        self.file: Optional[Any] = None
        self.path: Optional[Path] = None
        self.bytes = 0
        self.records = 0
        self.oversized_records = 0
        self.index = 0
        self.descriptors: List[Dict[str, Any]] = []
        self.total_records = 0
        self.total_bytes = 0

    def _open(self) -> None:
        self.index += 1
        self.path = self.directory / f"{self.prefix}-{self.index:06d}.ndjson"
        self.file = self.path.open("wb")
        self.bytes = 0
        self.records = 0
        self.oversized_records = 0

    def _close(self) -> None:
        if self.file is None or self.path is None:
            return
        self.file.flush()
        os.fsync(self.file.fileno())
        self.file.close()
        descriptor = {
            "path": self.path.name,
            "record_count": self.records,
            "file_bytes": self.bytes,
            "sha256": sha256_file(self.path),
        }
        if self.oversized_records:
            descriptor["oversized_records"] = self.oversized_records
        self.descriptors.append(descriptor)
        self.total_bytes += self.bytes
        self.file = None
        self.path = None

    def write(self, value: Mapping[str, Any]) -> None:
        line = (json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
        if self.file is not None and self.bytes and self.bytes + len(line) > self.max_bytes:
            self._close()
        if self.file is None:
            self._open()
        assert self.file is not None
        self.file.write(line)
        self.bytes += len(line)
        self.records += 1
        self.total_records += 1
        if len(line) > self.max_bytes:
            self.oversized_records += 1

    def close(self) -> None:
        self._close()


def _write_manifest(path: Path, value: Mapping[str, Any]) -> None:
    data = (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode("utf-8")
    with path.open("wb") as output:
        output.write(data)
        output.flush()
        os.fsync(output.fileno())


def _fsync_directory(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _failure(relative_path: str, error: FileFailure) -> Dict[str, Any]:
    result: Dict[str, Any] = {
        "path": relative_path,
        "code": error.code,
        "message": error.message,
    }
    if error.extractor:
        result["extractor"] = error.extractor
    return result


def _source_snapshot_update(
    digest: Any, relative_path: str, source_hash: str, updated_at_unix_ms: int
) -> None:
    digest.update(relative_path.encode("utf-8"))
    digest.update(b"\0")
    digest.update(source_hash.encode("ascii"))
    digest.update(b"\0")
    digest.update(str(updated_at_unix_ms).encode("ascii"))
    digest.update(b"\n")


def _current_source_snapshot(settings: Settings) -> Tuple[str, int]:
    digest = hashlib.sha256()
    count = 0
    excluded = [settings.output_dir]
    for optional in (settings.passage_dir, settings.vector_dir, settings.index_dir):
        if optional is not None:
            excluded.append(optional)
    if settings.vector_dir is not None:
        excluded.append(settings.vector_dir.parent / f".{settings.vector_dir.name}.work")
    if settings.usage_log_path is not None:
        excluded.append(settings.usage_log_path)
    for path in _walk_sorted(
        settings.root,
        tuple(excluded),
        settings.exclude,
        settings.follow_symlinks,
    ):
        try:
            relative_path = normalize_relative_path(path, settings.root)
        except (ValueError, OSError) as error:
            raise LocalFilesError(f"cannot fingerprint input path {path.name}: {error}") from error
        if not _selected(relative_path, settings):
            continue
        try:
            updated_at_unix_ms = max(0, path.stat().st_mtime_ns // 1_000_000)
            source_hash = sha256_file(path)
        except FileFailure as error:
            source_hash = f"error-{error.code}"
            updated_at_unix_ms = -1
        except OSError:
            source_hash = "error-io_error"
            updated_at_unix_ms = -1
        _source_snapshot_update(digest, relative_path, source_hash, updated_at_unix_ms)
        count += 1
    return digest.hexdigest(), count


def convert(settings: Settings) -> Dict[str, Any]:
    if not settings.root.is_dir():
        raise LocalFilesError(f"input.root is not a directory: {settings.root}")
    if settings.output_dir.exists():
        raise LocalFilesError(f"output directory already exists: {settings.output_dir}")
    settings.output_dir.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=f".{settings.output_dir.name}.tmp.", dir=settings.output_dir.parent))
    documents = ShardWriter(stage, "documents", settings.shard_max_bytes)
    failures = ShardWriter(stage, "failures", settings.shard_max_bytes)
    successful_files = 0
    source_snapshot = hashlib.sha256()
    input_file_count = 0
    try:
        for path in _walk_sorted(
            settings.root,
            (settings.output_dir, stage),
            settings.exclude,
            settings.follow_symlinks,
        ):
            try:
                relative_path = normalize_relative_path(path, settings.root)
            except (ValueError, OSError) as error:
                failures.write(_failure(path.name, FileFailure("path_error", str(error))))
                continue
            if not _selected(relative_path, settings):
                continue
            input_file_count += 1
            snapshot_recorded = False
            try:
                stat = path.stat()
                updated_at_unix_ms = max(0, stat.st_mtime_ns // 1_000_000)
                source_hash = sha256_file(path)
                _source_snapshot_update(
                    source_snapshot, relative_path, source_hash, updated_at_unix_ms
                )
                snapshot_recorded = True
                text, extractor = extract_file(path, relative_path, settings)
                if not text.strip():
                    raise FileFailure("no_text", "extractor returned no searchable text", extractor)
                parts = split_body(text, settings.body_max_bytes)
                if not parts:
                    raise FileFailure("no_text", "extractor returned no searchable text", extractor)
                mime = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
                part_count = len(parts)
                for index, body in enumerate(parts):
                    metadata = {
                        "collection_id": settings.collection_id,
                        "source_path": relative_path,
                        "source_sha256": source_hash,
                        "source_mime": mime,
                        "extractor": extractor,
                        "part_index": index,
                        "part_count": part_count,
                    }
                    documents.write(
                        {
                            "operation": "upsert",
                            "id": document_id(settings.collection_id, relative_path, index),
                            "title": truncate_utf8(relative_path, 255),
                            "body": body,
                            "metadata": metadata,
                            "updated_at_unix_ms": updated_at_unix_ms,
                        }
                    )
                successful_files += 1
            except FileFailure as error:
                if not snapshot_recorded:
                    _source_snapshot_update(
                        source_snapshot, relative_path, f"error-{error.code}", -1
                    )
                failures.write(_failure(relative_path, error))
            except OSError as error:
                if not snapshot_recorded:
                    _source_snapshot_update(
                        source_snapshot, relative_path, "error-io_error", -1
                    )
                failures.write(_failure(relative_path, FileFailure("io_error", str(error))))
        documents.close()
        failures.close()
        if documents.total_records == 0:
            raise LocalFilesError("no source file produced a canonical document")
        manifest = {
            "schema_version": SCHEMA_VERSION,
            "stage": "convert",
            "target": "documents",
            "collection_id": settings.collection_id,
            "config_fingerprint": settings.config_fingerprint,
            "source_manifest_sha256": None,
            "input_snapshot_sha256": source_snapshot.hexdigest(),
            "input_file_count": input_file_count,
            "successful_files": successful_files,
            "total_records": documents.total_records,
            "total_bytes": documents.total_bytes,
            "shards": documents.descriptors,
            "failure_count": failures.total_records,
            "failure_shards": failures.descriptors,
        }
        _write_manifest(stage / "manifest.json", manifest)
        _fsync_directory(stage)
        os.replace(stage, settings.output_dir)
        _fsync_directory(settings.output_dir.parent)
        return manifest
    except Exception:
        documents.close()
        failures.close()
        shutil.rmtree(stage, ignore_errors=True)
        raise


def _load_document_manifest(
    settings: Settings,
    directory: Path,
    expected_stage: str,
    expected_target: str,
    expected_config_fingerprint: Optional[str] = None,
) -> Tuple[Dict[str, Any], List[Path]]:
    manifest_path = directory / "manifest.json"
    try:
        manifest_value = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise LocalFilesError(f"cannot load documents manifest {manifest_path}: {error}") from error
    if not isinstance(manifest_value, dict):
        raise LocalFilesError("documents manifest root must be an object")
    manifest: Dict[str, Any] = manifest_value
    if manifest.get("schema_version") != SCHEMA_VERSION or manifest.get("stage") != expected_stage:
        raise LocalFilesError("documents manifest schema or stage does not match")
    if manifest.get("target") != expected_target:
        raise LocalFilesError(f"documents manifest target must be {expected_target}")
    if manifest.get("collection_id") != settings.collection_id:
        raise LocalFilesError("documents manifest collection_id does not match config")
    fingerprint = expected_config_fingerprint or settings.config_fingerprint
    if manifest.get("config_fingerprint") != fingerprint:
        raise LocalFilesError("documents manifest config fingerprint does not match config")
    descriptors = manifest.get("shards")
    if not isinstance(descriptors, list) or not descriptors:
        raise LocalFilesError("documents manifest has no shards")

    shard_paths: List[Path] = []
    record_total = 0
    byte_total = 0
    for ordinal, descriptor in enumerate(descriptors, 1):
        if not isinstance(descriptor, dict):
            raise LocalFilesError("documents manifest shard descriptor must be an object")
        name = descriptor.get("path")
        expected_name = f"documents-{ordinal:06d}.ndjson"
        if name != expected_name:
            raise LocalFilesError(
                f"documents manifest shard order is invalid: expected {expected_name!r}"
            )
        expected_records = descriptor.get("record_count")
        expected_bytes = descriptor.get("file_bytes")
        expected_sha = descriptor.get("sha256")
        if (
            not isinstance(expected_records, int)
            or isinstance(expected_records, bool)
            or expected_records <= 0
            or not isinstance(expected_bytes, int)
            or isinstance(expected_bytes, bool)
            or expected_bytes <= 0
            or not isinstance(expected_sha, str)
            or re.fullmatch(r"[0-9a-f]{64}", expected_sha) is None
        ):
            raise LocalFilesError(f"invalid shard descriptor for {expected_name}")
        shard_path = directory / expected_name
        try:
            actual_bytes = shard_path.stat().st_size
            with shard_path.open("rb") as source:
                actual_records = 0
                for line in source:
                    if not line.strip():
                        raise LocalFilesError(f"empty NDJSON record in {expected_name}")
                    actual_records += 1
            actual_sha = sha256_file(shard_path)
        except OSError as error:
            raise LocalFilesError(f"cannot verify document shard {shard_path}: {error}") from error
        except FileFailure as error:
            raise LocalFilesError(f"cannot verify document shard {shard_path}: {error.message}") from error
        if (
            actual_records != expected_records
            or actual_bytes != expected_bytes
            or actual_sha != expected_sha
        ):
            raise LocalFilesError(f"document shard verification failed: {expected_name}")
        shard_paths.append(shard_path)
        record_total += actual_records
        byte_total += actual_bytes
    if manifest.get("total_records") != record_total or manifest.get("total_bytes") != byte_total:
        raise LocalFilesError("documents manifest totals do not match shards")
    return manifest, shard_paths


def _load_documents_manifest(settings: Settings) -> Tuple[Dict[str, Any], List[Path]]:
    manifest, paths = _load_document_manifest(
        settings, settings.output_dir, "convert", "documents"
    )
    snapshot, input_file_count = _current_source_snapshot(settings)
    if (
        manifest.get("input_snapshot_sha256") != snapshot
        or manifest.get("input_file_count") != input_file_count
    ):
        raise LocalFilesError(
            "documents manifest input snapshot does not match current source files; "
            "remove the complete output tree before regeneration"
        )
    return manifest, paths


def _file_sha256_for_manifest(path: Path) -> str:
    try:
        return sha256_file(path)
    except FileFailure as error:
        raise LocalFilesError(f"cannot checksum {path}: {error.message}") from error


def _prepare_fingerprint(settings: Settings, index_config: Path) -> str:
    digest = hashlib.sha256()
    digest.update(settings.config_fingerprint.encode("ascii"))
    digest.update(b"\0prepare\0")
    digest.update(_file_sha256_for_manifest(index_config).encode("ascii"))
    return digest.hexdigest()


def _consume_passage_fifo(
    reader: Any, writer: ShardWriter, errors: List[BaseException]
) -> None:
    for line_number, raw_line in enumerate(reader, 1):
        if errors:
            continue
        try:
            line = raw_line.decode("utf-8")
            value = json.loads(line)
            if not isinstance(value, dict) or value.get("operation") != "upsert":
                raise LocalFilesError("prepare output must contain passage upserts")
            document_id = value.get("document_id")
            passage_id = value.get("passage_id")
            ordinal = value.get("ordinal")
            text = value.get("text")
            if (
                not isinstance(document_id, str)
                or not document_id
                or not isinstance(passage_id, str)
                or not passage_id
                or not isinstance(ordinal, int)
                or isinstance(ordinal, bool)
                or ordinal < 0
                or not isinstance(text, str)
                or not text.strip()
            ):
                raise LocalFilesError("prepare output contains an invalid passage")
            writer.write(value)
        except (UnicodeDecodeError, json.JSONDecodeError, LocalFilesError) as error:
            errors.append(LocalFilesError(f"invalid prepare output at line {line_number}: {error}"))


def _run_prepare_shard(
    yappo_makeindex: Path,
    index_config: Path,
    document_shard: Path,
    stage: Path,
    ordinal: int,
    writer: ShardWriter,
) -> None:
    fifo_path = stage / f"prepare-{ordinal:06d}.fifo"
    stdout_path = stage / f"prepare-{ordinal:06d}.stdout"
    stderr_path = stage / f"prepare-{ordinal:06d}.stderr"
    os.mkfifo(fifo_path, 0o600)
    reader_fd = os.open(fifo_path, os.O_RDONLY | os.O_NONBLOCK)
    dummy_writer_fd = os.open(fifo_path, os.O_WRONLY | os.O_NONBLOCK)
    os.set_blocking(reader_fd, True)
    reader = os.fdopen(reader_fd, "rb", buffering=0)
    errors: List[BaseException] = []
    consumer = threading.Thread(
        target=_consume_passage_fifo,
        args=(reader, writer, errors),
        name=f"local-files-passage-consumer-{ordinal}",
        daemon=True,
    )
    process: Optional[subprocess.Popen[Any]] = None
    try:
        consumer.start()
        with stdout_path.open("wb") as stdout_file, stderr_path.open("wb") as stderr_file:
            try:
                process = subprocess.Popen(
                    [
                        str(yappo_makeindex),
                        "prepare",
                        "--config",
                        str(index_config),
                        "--input",
                        str(document_shard),
                        "--output",
                        str(fifo_path),
                    ],
                    stdin=subprocess.DEVNULL,
                    stdout=stdout_file,
                    stderr=stderr_file,
                    shell=False,
                )
            except OSError as error:
                raise LocalFilesError(f"cannot start yappo_makeindex prepare: {error}") from error
            return_code = process.wait()
        os.close(dummy_writer_fd)
        dummy_writer_fd = -1
        consumer.join(timeout=10.0)
        if consumer.is_alive():
            raise LocalFilesError("prepare FIFO consumer did not stop")
        if errors:
            raise LocalFilesError(str(errors[0]))
        if return_code != 0:
            stderr = stderr_path.read_text(encoding="utf-8", errors="replace").strip()
            detail = f": {stderr[-4000:]}" if stderr else ""
            raise LocalFilesError(f"yappo_makeindex prepare exited with {return_code}{detail}")
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait()
        if dummy_writer_fd >= 0:
            os.close(dummy_writer_fd)
        reader.close()
        consumer.join(timeout=1.0)
        for path in (fifo_path, stdout_path, stderr_path):
            try:
                path.unlink()
            except FileNotFoundError:
                pass


def prepare_passages(settings: Settings) -> Dict[str, Any]:
    if settings.passage_dir is None:
        raise LocalFilesError("prepare.directory is required")
    if settings.yappo_makeindex is None:
        raise LocalFilesError("build.yappo_makeindex is required")
    if settings.index_config is None:
        raise LocalFilesError("build.index_config is required for prepare")
    if not settings.yappo_makeindex.is_file() or not os.access(settings.yappo_makeindex, os.X_OK):
        raise LocalFilesError(f"yappo_makeindex is not executable: {settings.yappo_makeindex}")
    if not settings.index_config.is_file():
        raise LocalFilesError(f"index config does not exist: {settings.index_config}")
    if settings.passage_dir.exists():
        raise LocalFilesError(f"passage directory already exists: {settings.passage_dir}")
    documents_manifest, document_shards = _load_documents_manifest(settings)
    settings.passage_dir.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(
        tempfile.mkdtemp(
            prefix=f".{settings.passage_dir.name}.tmp.", dir=settings.passage_dir.parent
        )
    )
    passages = ShardWriter(stage, "passages", settings.shard_max_bytes)
    try:
        for ordinal, document_shard in enumerate(document_shards, 1):
            _run_prepare_shard(
                settings.yappo_makeindex,
                settings.index_config,
                document_shard,
                stage,
                ordinal,
                passages,
            )
        passages.close()
        if passages.total_records == 0:
            raise LocalFilesError("prepare produced no passages")
        manifest = {
            "schema_version": SCHEMA_VERSION,
            "stage": "prepare",
            "target": "rag",
            "collection_id": settings.collection_id,
            "config_fingerprint": _prepare_fingerprint(settings, settings.index_config),
            "source_manifest_sha256": _file_sha256_for_manifest(
                settings.output_dir / "manifest.json"
            ),
            "total_records": passages.total_records,
            "total_bytes": passages.total_bytes,
            "shards": passages.descriptors,
            "failure_count": 0,
            "failure_shards": [],
        }
        if documents_manifest["total_records"] <= 0:
            raise LocalFilesError("documents manifest is empty")
        _write_manifest(stage / "manifest.json", manifest)
        _fsync_directory(stage)
        os.replace(stage, settings.passage_dir)
        _fsync_directory(settings.passage_dir.parent)
        return manifest
    except Exception:
        passages.close()
        shutil.rmtree(stage, ignore_errors=True)
        raise


def _load_passage_manifest(settings: Settings) -> Tuple[Dict[str, Any], List[Path]]:
    if settings.passage_dir is None:
        raise LocalFilesError("prepare.directory is required")
    manifest_path = settings.passage_dir / "manifest.json"
    try:
        value = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise LocalFilesError(f"cannot load passage manifest {manifest_path}: {error}") from error
    if not isinstance(value, dict):
        raise LocalFilesError("passage manifest root must be an object")
    if value.get("schema_version") != SCHEMA_VERSION or value.get("stage") != "prepare":
        raise LocalFilesError("passage manifest schema or stage does not match")
    if value.get("target") != "rag" or value.get("collection_id") != settings.collection_id:
        raise LocalFilesError("passage manifest target or collection does not match")
    if settings.index_config is None:
        raise LocalFilesError("build.index_config is required")
    if value.get("config_fingerprint") != _prepare_fingerprint(settings, settings.index_config):
        raise LocalFilesError("passage manifest config fingerprint does not match config")
    expected_source = _file_sha256_for_manifest(settings.output_dir / "manifest.json")
    if value.get("source_manifest_sha256") != expected_source:
        raise LocalFilesError("passage manifest source does not match documents manifest")
    descriptors = value.get("shards")
    if not isinstance(descriptors, list) or not descriptors:
        raise LocalFilesError("passage manifest has no shards")
    paths: List[Path] = []
    total_records = 0
    total_bytes = 0
    for ordinal, descriptor in enumerate(descriptors, 1):
        expected_name = f"passages-{ordinal:06d}.ndjson"
        if not isinstance(descriptor, dict) or descriptor.get("path") != expected_name:
            raise LocalFilesError("passage manifest shard order is invalid")
        expected_records = descriptor.get("record_count")
        expected_bytes = descriptor.get("file_bytes")
        expected_sha = descriptor.get("sha256")
        if (
            not isinstance(expected_records, int)
            or isinstance(expected_records, bool)
            or expected_records <= 0
            or not isinstance(expected_bytes, int)
            or isinstance(expected_bytes, bool)
            or expected_bytes <= 0
            or not isinstance(expected_sha, str)
            or re.fullmatch(r"[0-9a-f]{64}", expected_sha) is None
        ):
            raise LocalFilesError(f"invalid passage descriptor for {expected_name}")
        path = settings.passage_dir / expected_name
        try:
            actual_bytes = path.stat().st_size
            with path.open("rb") as source:
                actual_records = 0
                for line in source:
                    if not line.strip():
                        raise LocalFilesError(f"empty NDJSON record in {expected_name}")
                    actual_records += 1
            actual_sha = sha256_file(path)
        except (OSError, FileFailure) as error:
            raise LocalFilesError(f"cannot verify passage shard {path}: {error}") from error
        if (
            actual_records != expected_records
            or actual_bytes != expected_bytes
            or actual_sha != expected_sha
        ):
            raise LocalFilesError(f"passage shard verification failed: {expected_name}")
        paths.append(path)
        total_records += actual_records
        total_bytes += actual_bytes
    if value.get("total_records") != total_records or value.get("total_bytes") != total_bytes:
        raise LocalFilesError("passage manifest totals do not match shards")
    return value, paths


def _iter_ndjson(paths: Sequence[Path], label: str) -> Iterator[Dict[str, Any]]:
    for path in paths:
        try:
            with path.open("r", encoding="utf-8") as source:
                for line_number, line in enumerate(source, 1):
                    if not line.strip():
                        raise LocalFilesError(f"empty {label} record at {path}:{line_number}")
                    try:
                        value = json.loads(line)
                    except json.JSONDecodeError as error:
                        raise LocalFilesError(
                            f"invalid {label} JSON at {path}:{line_number}"
                        ) from error
                    if not isinstance(value, dict):
                        raise LocalFilesError(f"{label} record must be an object")
                    yield value
        except (OSError, UnicodeDecodeError) as error:
            raise LocalFilesError(f"cannot read {label} shard {path}: {error}") from error


class _PassageCursor:
    def __init__(self, paths: Sequence[Path]):
        self.iterator = iter(_iter_ndjson(paths, "passage"))
        self.current: Optional[Dict[str, Any]] = None
        self.exhausted = False

    def _advance(self) -> None:
        try:
            self.current = next(self.iterator)
        except StopIteration:
            self.current = None
            self.exhausted = True

    def take(self, document_id: str) -> List[Dict[str, Any]]:
        if self.current is None and not self.exhausted:
            self._advance()
        if self.current is None or self.current.get("document_id") != document_id:
            raise LocalFilesError(f"prepared passages are missing or out of order for {document_id}")
        passages: List[Dict[str, Any]] = []
        while self.current is not None and self.current.get("document_id") == document_id:
            ordinal = self.current.get("ordinal")
            text = self.current.get("text")
            if (
                ordinal != len(passages)
                or not isinstance(ordinal, int)
                or isinstance(ordinal, bool)
                or not isinstance(text, str)
                or not text.strip()
            ):
                raise LocalFilesError(f"prepared passage ordinal or text is invalid for {document_id}")
            passages.append(self.current)
            self._advance()
        return passages

    def finish(self) -> None:
        if self.current is None and not self.exhausted:
            self._advance()
        if self.current is not None:
            raise LocalFilesError("passage manifest contains records for unknown documents")


def _post_embedding_json(config: EmbeddingConfig, inputs: Sequence[str]) -> Dict[str, Any]:
    payload = {"model": config.model, "input": list(inputs)}
    if config.provider == "openai":
        payload["dimensions"] = config.dimensions
    headers = {"Content-Type": "application/json", "Accept": "application/json"}
    if config.authorization_token:
        headers["Authorization"] = "Bearer " + config.authorization_token
    request = Request(
        config.endpoint_url,
        data=json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8"),
        headers=headers,
        method="POST",
    )
    try:
        with urlopen(request, timeout=config.timeout_ms / 1000.0) as response:
            value = json.loads(response.read().decode("utf-8"))
    except HTTPError as error:
        raise LocalFilesError(f"embedding server returned HTTP {error.code}") from error
    except (URLError, TimeoutError, OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise LocalFilesError(f"embedding request failed: {error}") from error
    if not isinstance(value, dict):
        raise LocalFilesError("embedding response root must be an object")
    _append_usage_log(
        config.usage_log_path,
        source="local-files",
        service="embedding",
        operation="batch_embed",
        provider=config.provider,
        model=config.model,
        usage=value.get("usage"),
    )
    return value


def _validated_vector(value: Any, dimensions: int) -> List[float]:
    if not isinstance(value, list) or len(value) != dimensions:
        raise LocalFilesError(f"embedding vector must contain {dimensions} values")
    if any(
        not isinstance(item, (int, float))
        or isinstance(item, bool)
        or not math.isfinite(item)
        for item in value
    ):
        raise LocalFilesError("embedding vector contains a non-finite number")
    return [float(item) for item in value]


def _embedding_batch(config: EmbeddingConfig, inputs: Sequence[str]) -> List[List[float]]:
    response = _post_embedding_json(config, inputs)
    if config.provider == "ollama":
        embeddings = response.get("embeddings")
        if not isinstance(embeddings, list) or len(embeddings) != len(inputs):
            raise LocalFilesError("Ollama embedding count does not match input count")
        return [_validated_vector(value, config.dimensions) for value in embeddings]
    data = response.get("data")
    if not isinstance(data, list) or len(data) != len(inputs):
        raise LocalFilesError("OpenAI-compatible embedding count does not match input count")
    ordered: List[Optional[List[float]]] = [None] * len(inputs)
    for item in data:
        if not isinstance(item, dict):
            raise LocalFilesError("embedding response item must be an object")
        index = item.get("index")
        if (
            not isinstance(index, int)
            or isinstance(index, bool)
            or index < 0
            or index >= len(inputs)
            or ordered[index] is not None
        ):
            raise LocalFilesError("embedding response indexes are invalid")
        ordered[index] = _validated_vector(item.get("embedding"), config.dimensions)
    if any(value is None for value in ordered):
        raise LocalFilesError("embedding response has missing indexes")
    return [value for value in ordered if value is not None]


def _validate_hybrid_index_config(settings: Settings, embedding: EmbeddingConfig) -> str:
    if settings.hybrid_index_config is None or not settings.hybrid_index_config.is_file():
        raise LocalFilesError("build.hybrid_index_config is required")
    try:
        data = tomllib.loads(settings.hybrid_index_config.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, tomllib.TOMLDecodeError) as error:
        raise LocalFilesError(f"cannot load hybrid index config: {error}") from error
    vector = data.get("vector") if isinstance(data, dict) else None
    if not isinstance(vector, dict) or vector.get("enabled") is not True:
        raise LocalFilesError("hybrid index config must enable vector search")
    if vector.get("model_id") != embedding.model_id:
        raise LocalFilesError("embedding.model_id must match hybrid vector.model_id")
    if vector.get("dimensions") != embedding.dimensions:
        raise LocalFilesError("embedding.dimensions must match hybrid vector.dimensions")
    if settings.index_config is None or not settings.index_config.is_file():
        raise LocalFilesError("build.index_config is required for hybrid passage preparation")
    try:
        prepare_data = tomllib.loads(settings.index_config.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, tomllib.TOMLDecodeError) as error:
        raise LocalFilesError(f"cannot load prepare index config: {error}") from error
    for section in ("tokenizer", "chunking"):
        if prepare_data.get(section) != data.get(section):
            raise LocalFilesError(
                f"lexical and hybrid index configs must use the same [{section}] settings"
            )
    return _file_sha256_for_manifest(settings.hybrid_index_config)


def _verify_checkpoint_shards(directory: Path, descriptors: Any) -> List[Path]:
    if not isinstance(descriptors, list) or not descriptors:
        raise LocalFilesError("embedding checkpoint has no output shards")
    paths: List[Path] = []
    for ordinal, descriptor in enumerate(descriptors, 1):
        name = f"documents-{ordinal:06d}.ndjson"
        if not isinstance(descriptor, dict) or descriptor.get("path") != name:
            raise LocalFilesError("embedding checkpoint shard order is invalid")
        path = directory / name
        expected_records = descriptor.get("record_count")
        expected_bytes = descriptor.get("file_bytes")
        expected_sha = descriptor.get("sha256")
        try:
            actual_bytes = path.stat().st_size
            with path.open("rb") as source:
                actual_records = 0
                for line in source:
                    if not line.strip():
                        raise LocalFilesError(f"empty NDJSON record in {name}")
                    actual_records += 1
            actual_sha = sha256_file(path)
        except (OSError, FileFailure) as error:
            raise LocalFilesError(f"cannot verify embedding checkpoint shard: {error}") from error
        if (
            actual_records != expected_records
            or actual_bytes != expected_bytes
            or actual_sha != expected_sha
        ):
            raise LocalFilesError(f"embedding checkpoint shard verification failed: {name}")
        paths.append(path)
    return paths


def _consume_document_shard_passages(
    document_shard: Path, passage_cursor: _PassageCursor
) -> Tuple[int, int]:
    document_count = 0
    passage_count = 0
    for document in _iter_ndjson((document_shard,), "document"):
        document_id = document.get("id")
        if document.get("operation") != "upsert" or not isinstance(document_id, str) or not document_id:
            raise LocalFilesError("vector input must contain canonical document upserts")
        passages = passage_cursor.take(document_id)
        document_count += 1
        passage_count += len(passages)
    return document_count, passage_count


def _document_group_inputs(
    document_shard: Path,
    passage_cursor: _PassageCursor,
    embedding: EmbeddingConfig,
) -> Tuple[List[Tuple[Dict[str, Any], List[Tuple[int, str, str]]]], List[Tuple[str, int, str, str]]]:
    documents: List[Tuple[Dict[str, Any], List[Tuple[int, str, str]]]] = []
    entries: List[Tuple[str, int, str, str]] = []
    for document in _iter_ndjson((document_shard,), "document"):
        document_id = document.get("id")
        if (
            document.get("operation") != "upsert"
            or not isinstance(document_id, str)
            or not document_id
        ):
            raise LocalFilesError("vector input must contain canonical document upserts")
        title = document.get("title")
        title_text = title.strip() if isinstance(title, str) and title.strip() else "none"
        prepared: List[Tuple[int, str, str]] = []
        for passage in passage_cursor.take(document_id):
            ordinal = int(passage["ordinal"])
            text = str(passage["text"])
            if embedding.prompt_profile == "embeddinggemma":
                text = f"title: {title_text} | text: {text}"
            input_sha = hashlib.sha256(text.encode("utf-8")).hexdigest()
            prepared.append((ordinal, text, input_sha))
            entries.append((document_id, ordinal, text, input_sha))
        documents.append((document, prepared))
    return documents, entries


def _load_embedding_journal(
    path: Path,
    entries: Sequence[Tuple[str, int, str, str]],
    dimensions: int,
) -> int:
    if not path.exists():
        return 0
    completed = 0
    try:
        with path.open("r+b") as source:
            while True:
                line_start = source.tell()
                line = source.readline()
                if not line:
                    break
                if not line.endswith(b"\n"):
                    source.truncate(line_start)
                    source.flush()
                    os.fsync(source.fileno())
                    break
                if completed >= len(entries):
                    raise LocalFilesError("embedding journal contains unexpected vectors")
                try:
                    value = json.loads(line.decode("utf-8"))
                except (UnicodeDecodeError, json.JSONDecodeError) as error:
                    raise LocalFilesError("embedding journal contains invalid JSON") from error
                document_id, ordinal, _text, input_sha = entries[completed]
                if (
                    not isinstance(value, dict)
                    or value.get("sequence") != completed
                    or value.get("document_id") != document_id
                    or value.get("ordinal") != ordinal
                    or value.get("input_sha256") != input_sha
                ):
                    raise LocalFilesError("embedding journal does not match prepared passages")
                _validated_vector(value.get("vector"), dimensions)
                completed += 1
    except OSError as error:
        raise LocalFilesError(f"cannot read embedding journal: {error}") from error
    return completed


def _append_embedding_journal_batch(
    path: Path,
    entries: Sequence[Tuple[str, int, str, str]],
    vectors: Sequence[Sequence[float]],
    start: int,
) -> None:
    records = []
    for offset, (entry, vector) in enumerate(zip(entries, vectors)):
        document_id, ordinal, _text, input_sha = entry
        records.append(
            json.dumps(
                {
                    "sequence": start + offset,
                    "document_id": document_id,
                    "ordinal": ordinal,
                    "input_sha256": input_sha,
                    "vector": list(vector),
                },
                ensure_ascii=False,
                separators=(",", ":"),
            ).encode("utf-8") + b"\n"
        )
    try:
        with path.open("ab") as output:
            output.write(b"".join(records))
            output.flush()
            os.fsync(output.fileno())
        _fsync_directory(path.parent)
    except OSError as error:
        raise LocalFilesError(f"cannot append embedding journal: {error}") from error


def _write_document_group_from_journal(
    output_dir: Path,
    journal_path: Path,
    documents: Sequence[Tuple[Dict[str, Any], List[Tuple[int, str, str]]]],
    shard_max_bytes: int,
    dimensions: int,
) -> Tuple[int, int, List[Dict[str, Any]]]:
    writer = ShardWriter(output_dir, "documents", shard_max_bytes)
    passage_count = 0
    try:
        with journal_path.open("r", encoding="utf-8") as source:
            for document, passages in documents:
                vectors: List[List[float]] = []
                for _passage in passages:
                    line = source.readline()
                    if not line:
                        raise LocalFilesError("embedding journal ended before all passages")
                    value = json.loads(line)
                    vectors.append(_validated_vector(value.get("vector"), dimensions))
                enriched = dict(document)
                enriched["vectors"] = vectors
                writer.write(enriched)
                passage_count += len(passages)
            if source.readline():
                raise LocalFilesError("embedding journal contains extra vectors")
        writer.close()
        return len(documents), passage_count, writer.descriptors
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        writer.close()
        raise LocalFilesError(f"cannot finalize embedding journal: {error}") from error
    except Exception:
        writer.close()
        raise


def _embed_document_group_resumable(
    document_shard: Path,
    passage_cursor: _PassageCursor,
    progress_dir: Path,
    output_dir: Path,
    shard_max_bytes: int,
    embedding: EmbeddingConfig,
    expected: Mapping[str, Any],
) -> Tuple[int, int, List[Dict[str, Any]]]:
    progress_path = progress_dir / "progress.json"
    journal_path = progress_dir / "vectors.ndjson"
    if progress_dir.exists():
        try:
            progress = json.loads(progress_path.read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
            raise LocalFilesError(f"cannot load embedding progress: {error}") from error
        if not isinstance(progress, dict) or any(progress.get(key) != value for key, value in expected.items()):
            raise LocalFilesError("embedding progress configuration mismatch")
    else:
        progress_dir.mkdir(parents=True)
        _write_manifest(progress_path, expected)
        _fsync_directory(progress_dir)

    documents, entries = _document_group_inputs(document_shard, passage_cursor, embedding)
    completed = _load_embedding_journal(journal_path, entries, embedding.dimensions)
    while completed < len(entries):
        batch = entries[completed:completed + embedding.batch_size]
        vectors = _embedding_batch(embedding, [entry[2] for entry in batch])
        if len(vectors) != len(batch):
            raise LocalFilesError("embedding count does not match passage count")
        _append_embedding_journal_batch(journal_path, batch, vectors, completed)
        completed += len(batch)
    return _write_document_group_from_journal(
        output_dir,
        journal_path,
        documents,
        shard_max_bytes,
        embedding.dimensions,
    )


def _embedding_checkpoint_expected(
    input_descriptor: Mapping[str, Any],
    passage_manifest_sha: str,
    chunk_config_sha: str,
    embedding: EmbeddingConfig,
) -> Dict[str, Any]:
    return {
        "schema_version": SCHEMA_VERSION,
        "input_shard_sha256": input_descriptor["sha256"],
        "passage_manifest_sha256": passage_manifest_sha,
        "chunk_config_sha256": chunk_config_sha,
        "provider": embedding.provider,
        "base_url": embedding.base_url,
        "endpoint_url": embedding.endpoint_url,
        "model": embedding.model,
        "model_id": embedding.model_id,
        "dimensions": embedding.dimensions,
        "prompt_profile": embedding.prompt_profile,
    }


def _embed_manifest_fingerprint(
    settings: Settings, hybrid_config_sha: str, passage_manifest_sha: str
) -> str:
    digest = hashlib.sha256()
    digest.update(settings.config_fingerprint.encode("ascii"))
    digest.update(b"\0embed\0")
    digest.update(hybrid_config_sha.encode("ascii"))
    digest.update(passage_manifest_sha.encode("ascii"))
    return digest.hexdigest()


def embed_documents(settings: Settings) -> Dict[str, Any]:
    if settings.vector_dir is None:
        raise LocalFilesError("embedding.directory is required")
    if settings.embedding is None:
        raise LocalFilesError("embedding configuration is required")
    if settings.vector_dir.exists():
        raise LocalFilesError(f"vector document directory already exists: {settings.vector_dir}")
    hybrid_config_sha = _validate_hybrid_index_config(settings, settings.embedding)
    documents_manifest, document_shards = _load_documents_manifest(settings)
    passage_manifest, passage_shards = _load_passage_manifest(settings)
    passage_manifest_path = settings.passage_dir / "manifest.json"  # type: ignore[operator]
    passage_manifest_sha = _file_sha256_for_manifest(passage_manifest_path)
    chunk_config_sha = _file_sha256_for_manifest(settings.index_config)  # type: ignore[arg-type]
    work_dir = settings.vector_dir.parent / f".{settings.vector_dir.name}.work"
    work_dir.mkdir(parents=True, exist_ok=True)
    expected_group_names = {f"input-{ordinal:06d}" for ordinal in range(1, len(document_shards) + 1)}
    extra_groups = {
        path.name for path in work_dir.iterdir() if path.is_dir() and path.name.startswith("input-")
    } - expected_group_names
    if extra_groups:
        raise LocalFilesError("embedding checkpoint contains unexpected input shards")

    passage_cursor = _PassageCursor(passage_shards)
    groups: List[Tuple[Path, Dict[str, Any]]] = []
    for ordinal, (document_shard, input_descriptor) in enumerate(
        zip(document_shards, documents_manifest["shards"]), 1
    ):
        group_dir = work_dir / f"input-{ordinal:06d}"
        progress_dir = work_dir / f".input-{ordinal:06d}.progress"
        expected = _embedding_checkpoint_expected(
            input_descriptor,
            passage_manifest_sha,
            chunk_config_sha,
            settings.embedding,
        )
        checkpoint_path = group_dir / "checkpoint.json"
        if group_dir.exists():
            try:
                checkpoint = json.loads(checkpoint_path.read_text(encoding="utf-8"))
            except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
                raise LocalFilesError(f"cannot load embedding checkpoint: {error}") from error
            if not isinstance(checkpoint, dict) or any(
                checkpoint.get(key) != value for key, value in expected.items()
            ):
                raise LocalFilesError(
                    f"embedding checkpoint configuration mismatch for input shard {ordinal}"
                )
            _verify_checkpoint_shards(group_dir, checkpoint.get("output_shards"))
            document_count, passage_count = _consume_document_shard_passages(
                document_shard, passage_cursor
            )
            if (
                checkpoint.get("document_count") != document_count
                or checkpoint.get("passage_count") != passage_count
            ):
                raise LocalFilesError(f"embedding checkpoint counts mismatch for input shard {ordinal}")
            groups.append((group_dir, checkpoint))
            continue

        temporary_group = Path(tempfile.mkdtemp(prefix=f".input-{ordinal:06d}.finalize.", dir=work_dir))
        try:
            document_count, passage_count, descriptors = _embed_document_group_resumable(
                document_shard,
                passage_cursor,
                progress_dir,
                temporary_group,
                settings.shard_max_bytes,
                settings.embedding,
                expected,
            )
            checkpoint = dict(expected)
            checkpoint.update(
                {
                    "document_count": document_count,
                    "passage_count": passage_count,
                    "output_shards": descriptors,
                }
            )
            _write_manifest(temporary_group / "checkpoint.json", checkpoint)
            _fsync_directory(temporary_group)
            os.replace(temporary_group, group_dir)
            _fsync_directory(work_dir)
            shutil.rmtree(progress_dir, ignore_errors=True)
            groups.append((group_dir, checkpoint))
        except Exception:
            shutil.rmtree(temporary_group, ignore_errors=True)
            raise
    passage_cursor.finish()

    settings.vector_dir.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(
        tempfile.mkdtemp(prefix=f".{settings.vector_dir.name}.tmp.", dir=settings.vector_dir.parent)
    )
    documents = ShardWriter(stage, "documents", settings.shard_max_bytes)
    try:
        for group_dir, checkpoint in groups:
            group_shards = _verify_checkpoint_shards(group_dir, checkpoint["output_shards"])
            for record in _iter_ndjson(group_shards, "vector document"):
                documents.write(record)
        documents.close()
        if documents.total_records != documents_manifest["total_records"]:
            raise LocalFilesError("vector document count does not match source documents")
        manifest = {
            "schema_version": SCHEMA_VERSION,
            "stage": "embed",
            "target": "hybrid",
            "collection_id": settings.collection_id,
            "config_fingerprint": _embed_manifest_fingerprint(
                settings, hybrid_config_sha, passage_manifest_sha
            ),
            "source_manifest_sha256": passage_manifest_sha,
            "total_records": documents.total_records,
            "total_bytes": documents.total_bytes,
            "shards": documents.descriptors,
            "failure_count": 0,
            "failure_shards": [],
            "model_id": settings.embedding.model_id,
            "dimensions": settings.embedding.dimensions,
            "passage_count": passage_manifest["total_records"],
        }
        _write_manifest(stage / "manifest.json", manifest)
        _fsync_directory(stage)
        os.replace(stage, settings.vector_dir)
        _fsync_directory(settings.vector_dir.parent)
        shutil.rmtree(work_dir, ignore_errors=True)
        return manifest
    except Exception:
        documents.close()
        shutil.rmtree(stage, ignore_errors=True)
        raise


def _require_build_settings(
    settings: Settings, target: str, *, allow_existing: bool = False
) -> Tuple[Path, Path, Path]:
    if target not in {"lexical", "rag", "hybrid"}:
        raise LocalFilesError("build target must be lexical, rag, or hybrid")
    if settings.yappo_makeindex is None:
        raise LocalFilesError("build.yappo_makeindex is required")
    index_config = settings.hybrid_index_config if target == "hybrid" else settings.index_config
    if index_config is None:
        name = "build.hybrid_index_config" if target == "hybrid" else "build.index_config"
        raise LocalFilesError(f"{name} is required")
    if settings.index_dir is None:
        raise LocalFilesError("build.index_directory is required")
    if not settings.yappo_makeindex.is_file() or not os.access(settings.yappo_makeindex, os.X_OK):
        raise LocalFilesError(f"yappo_makeindex is not executable: {settings.yappo_makeindex}")
    if not index_config.is_file():
        raise LocalFilesError(f"index config does not exist: {index_config}")
    if settings.index_dir.exists() and not allow_existing:
        raise LocalFilesError(f"index directory already exists: {settings.index_dir}")
    try:
        settings.index_dir.relative_to(settings.output_dir)
    except ValueError:
        pass
    else:
        raise LocalFilesError("build.index_directory must not be inside output.directory")
    return settings.yappo_makeindex, index_config, settings.index_dir


def _stream_document_shards(fifo_path: Path, shard_paths: Sequence[Path], errors: List[BaseException]) -> None:
    try:
        with fifo_path.open("wb", buffering=0) as output:
            for shard_path in shard_paths:
                with shard_path.open("rb") as source:
                    shutil.copyfileobj(source, output, length=1024 * 1024)
    except BaseException as error:
        errors.append(error)


def _verify_built_index(
    index: Path,
    target: str,
    generation: int,
    accepted: int,
    source_config: Path,
) -> None:
    copied_config = index / "config.toml"
    manifest_path = index / "manifest.json"
    if not copied_config.is_file() or not manifest_path.is_file():
        raise LocalFilesError("built index is missing config.toml or manifest.json")
    if _file_sha256_for_manifest(copied_config) != _file_sha256_for_manifest(source_config):
        raise LocalFilesError("built index config does not match requested config")
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise LocalFilesError(f"cannot load built index manifest: {error}") from error
    if not isinstance(manifest, dict) or manifest.get("generation") != generation:
        raise LocalFilesError("built index manifest generation does not match build result")
    segments = manifest.get("segments")
    if not isinstance(segments, list) or not segments:
        raise LocalFilesError("built index manifest has no segments")
    required = {
        "documents.yap2",
        "terms.yap2",
        "postings.yap2",
        "positions.yap2",
        "metadata.yap2",
    }
    if target == "hybrid":
        required.update({"vectors.yap2", "vectors.usearch"})
    document_total = 0
    for descriptor in segments:
        if not isinstance(descriptor, dict):
            raise LocalFilesError("built index segment descriptor must be an object")
        segment_id = descriptor.get("id")
        documents = descriptor.get("documents")
        components = descriptor.get("components")
        if (
            not isinstance(segment_id, str)
            or not segment_id
            or Path(segment_id).name != segment_id
            or not isinstance(documents, int)
            or isinstance(documents, bool)
            or documents <= 0
            or not isinstance(components, list)
        ):
            raise LocalFilesError("built index segment descriptor is invalid")
        segment_dir = index / "segments" / segment_id
        if not segment_dir.is_dir():
            raise LocalFilesError(f"built index segment directory is missing: {segment_id}")
        names = set()
        for component in components:
            if not isinstance(component, dict):
                raise LocalFilesError("built index component descriptor must be an object")
            name = component.get("name")
            file_bytes = component.get("file_bytes")
            checksum = component.get("sha256")
            if (
                not isinstance(name, str)
                or not name
                or Path(name).name != name
                or name in names
                or not isinstance(file_bytes, int)
                or isinstance(file_bytes, bool)
                or file_bytes <= 0
                or not isinstance(checksum, str)
                or re.fullmatch(r"[0-9a-f]{64}", checksum) is None
            ):
                raise LocalFilesError("built index component descriptor is invalid")
            component_path = segment_dir / name
            try:
                actual_bytes = component_path.stat().st_size
            except OSError as error:
                raise LocalFilesError(f"cannot stat built index component: {error}") from error
            if actual_bytes != file_bytes or _file_sha256_for_manifest(component_path) != checksum:
                raise LocalFilesError(f"built index component verification failed: {name}")
            names.add(name)
        if not required.issubset(names):
            missing = ", ".join(sorted(required - names))
            raise LocalFilesError(f"built index segment is missing components: {missing}")
        document_total += documents
    if document_total != accepted:
        raise LocalFilesError(
            f"built index contains {document_total} documents, expected {accepted}"
        )


def _build_source_manifest_path(settings: Settings, target: str) -> Path:
    if target == "hybrid":
        if settings.vector_dir is None:
            raise LocalFilesError("embedding.directory is required")
        return settings.vector_dir / "manifest.json"
    return settings.output_dir / "manifest.json"


def _reuse_built_index(settings: Settings, target: str) -> Dict[str, Any]:
    _yappo_makeindex, index_config, index_dir = _require_build_settings(
        settings, target, allow_existing=True
    )
    state_path = index_dir / BUILD_STATE_FILENAME
    try:
        state = json.loads(state_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise LocalFilesError(f"cannot reuse existing index without valid {BUILD_STATE_FILENAME}: {error}") from error
    expected_source_sha = _file_sha256_for_manifest(_build_source_manifest_path(settings, target))
    expected_config_sha = _file_sha256_for_manifest(index_config)
    if (
        not isinstance(state, dict)
        or state.get("schema_version") != SCHEMA_VERSION
        or state.get("stage") != "build"
        or state.get("target") != target
        or state.get("source_manifest_sha256") != expected_source_sha
        or state.get("index_config_sha256") != expected_config_sha
    ):
        raise LocalFilesError("existing index build state does not match requested inputs")
    generation = state.get("generation")
    accepted = state.get("accepted")
    if (
        not isinstance(generation, int)
        or isinstance(generation, bool)
        or generation <= 0
        or not isinstance(accepted, int)
        or isinstance(accepted, bool)
        or accepted <= 0
    ):
        raise LocalFilesError("existing index build state is invalid")
    _verify_built_index(index_dir, target, generation, accepted, index_config)
    return {
        "schema_version": SCHEMA_VERSION,
        "stage": "build",
        "target": target,
        "generation": generation,
        "accepted": accepted,
        "index": str(index_dir),
    }


def build_index(settings: Settings, target: str) -> Dict[str, Any]:
    yappo_makeindex, index_config, index_dir = _require_build_settings(settings, target)
    if target == "hybrid":
        if settings.vector_dir is None or settings.embedding is None:
            raise LocalFilesError("hybrid build requires embedding.directory and configuration")
        hybrid_config_sha = _validate_hybrid_index_config(settings, settings.embedding)
        if settings.passage_dir is None:
            raise LocalFilesError("prepare.directory is required")
        passage_manifest_sha = _file_sha256_for_manifest(settings.passage_dir / "manifest.json")
        manifest, shard_paths = _load_document_manifest(
            settings,
            settings.vector_dir,
            "embed",
            "hybrid",
            _embed_manifest_fingerprint(settings, hybrid_config_sha, passage_manifest_sha),
        )
    else:
        manifest, shard_paths = _load_documents_manifest(settings)
        if target == "rag":
            _load_passage_manifest(settings)
    index_dir.parent.mkdir(parents=True, exist_ok=True)
    stage_root = Path(tempfile.mkdtemp(prefix=f".{index_dir.name}.tmp.", dir=index_dir.parent))
    stage_index = stage_root / "index"
    fifo_path = stage_root / "documents.ndjson.fifo"
    stdout_path = stage_root / "build.stdout"
    stderr_path = stage_root / "build.stderr"
    producer_errors: List[BaseException] = []
    producer: Optional[threading.Thread] = None
    process: Optional[subprocess.Popen[Any]] = None

    def drain_failed_producer() -> None:
        if producer is None or not producer.is_alive():
            return
        rescue_fd = os.open(fifo_path, os.O_RDONLY | os.O_NONBLOCK)
        deadline = time.monotonic() + 5.0
        try:
            while producer.is_alive() and time.monotonic() < deadline:
                try:
                    if not os.read(rescue_fd, 1024 * 1024):
                        time.sleep(0.01)
                except BlockingIOError:
                    time.sleep(0.01)
                producer.join(timeout=0.01)
        finally:
            os.close(rescue_fd)

    try:
        os.mkfifo(fifo_path, 0o600)
        with stdout_path.open("wb") as stdout_file, stderr_path.open("wb") as stderr_file:
            try:
                process = subprocess.Popen(
                    [
                        str(yappo_makeindex),
                        "build",
                        "--config",
                        str(index_config),
                        "--input",
                        str(fifo_path),
                        "--index",
                        str(stage_index),
                    ],
                    stdin=subprocess.DEVNULL,
                    stdout=stdout_file,
                    stderr=stderr_file,
                    shell=False,
                )
            except OSError as error:
                raise LocalFilesError(f"cannot start yappo_makeindex: {error}") from error
            producer = threading.Thread(
                target=_stream_document_shards,
                args=(fifo_path, shard_paths, producer_errors),
                name="local-files-shard-producer",
                daemon=True,
            )
            producer.start()
            while producer.is_alive():
                if process.poll() is not None:
                    drain_failed_producer()
                    break
                producer.join(timeout=0.05)
            if producer.is_alive():
                process.kill()
                process.wait()
                raise LocalFilesError("document shard producer did not stop after build exited")
            if producer_errors:
                if process.poll() is None:
                    process.kill()
                process.wait()
                raise LocalFilesError(f"document shard producer failed: {producer_errors[0]}")
            return_code = process.wait()
            stdout_file.flush()
            stderr_file.flush()

        try:
            stdout = stdout_path.read_text(encoding="utf-8").strip()
            stderr = stderr_path.read_text(encoding="utf-8", errors="replace").strip()
        except OSError as error:
            raise LocalFilesError(f"cannot read yappo_makeindex result: {error}") from error
        if return_code != 0:
            detail = f": {stderr[-4000:]}" if stderr else ""
            raise LocalFilesError(f"yappo_makeindex build exited with {return_code}{detail}")
        lines = [line for line in stdout.splitlines() if line.strip()]
        try:
            summary = json.loads(lines[-1])
        except (IndexError, json.JSONDecodeError) as error:
            raise LocalFilesError("yappo_makeindex did not return a valid build summary") from error
        if not isinstance(summary, dict):
            raise LocalFilesError("yappo_makeindex build summary must be an object")
        accepted = summary.get("accepted")
        generation = summary.get("generation")
        if accepted != manifest["total_records"]:
            raise LocalFilesError(
                f"yappo_makeindex accepted {accepted!r}, expected {manifest['total_records']}"
            )
        if not isinstance(generation, int) or isinstance(generation, bool) or generation <= 0:
            raise LocalFilesError("yappo_makeindex returned an invalid generation")
        _verify_built_index(
            stage_index,
            target,
            generation,
            accepted,
            index_config,
        )
        _write_manifest(
            stage_index / BUILD_STATE_FILENAME,
            {
                "schema_version": SCHEMA_VERSION,
                "stage": "build",
                "target": target,
                "generation": generation,
                "accepted": accepted,
                "source_manifest_sha256": _file_sha256_for_manifest(
                    _build_source_manifest_path(settings, target)
                ),
                "index_config_sha256": _file_sha256_for_manifest(index_config),
            },
        )
        _fsync_directory(stage_index)
        os.replace(stage_index, index_dir)
        _fsync_directory(index_dir.parent)
        return {
            "schema_version": SCHEMA_VERSION,
            "stage": "build",
            "target": target,
            "generation": generation,
            "accepted": accepted,
            "index": str(index_dir),
        }
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait()
        if producer is not None and producer.is_alive():
            drain_failed_producer()
        shutil.rmtree(stage_root, ignore_errors=True)


def _load_vector_manifest(settings: Settings) -> Dict[str, Any]:
    if settings.vector_dir is None or settings.embedding is None:
        raise LocalFilesError("hybrid target requires embedding configuration")
    hybrid_config_sha = _validate_hybrid_index_config(settings, settings.embedding)
    if settings.passage_dir is None:
        raise LocalFilesError("prepare.directory is required")
    passage_manifest_sha = _file_sha256_for_manifest(settings.passage_dir / "manifest.json")
    manifest, _paths = _load_document_manifest(
        settings,
        settings.vector_dir,
        "embed",
        "hybrid",
        _embed_manifest_fingerprint(settings, hybrid_config_sha, passage_manifest_sha),
    )
    return manifest


def run_all(settings: Settings, target: str) -> Dict[str, Any]:
    if target not in {"documents", "lexical", "rag", "hybrid"}:
        raise LocalFilesError(f"unknown target {target!r}")
    if settings.output_dir.exists():
        documents, _document_paths = _load_documents_manifest(settings)
    else:
        documents = convert(settings)
    result: Dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "stage": "all",
        "target": target,
        "documents": {
            "total_records": documents["total_records"],
            "total_bytes": documents["total_bytes"],
            "failure_count": documents["failure_count"],
            "shards": documents["shards"],
        },
    }
    if target in {"rag", "hybrid"}:
        if settings.passage_dir is not None and settings.passage_dir.exists():
            passages, _passage_paths = _load_passage_manifest(settings)
        else:
            passages = prepare_passages(settings)
        result["passages"] = {
            "total_records": passages["total_records"],
            "total_bytes": passages["total_bytes"],
            "shards": passages["shards"],
        }
    if target == "hybrid":
        if settings.vector_dir is not None and settings.vector_dir.exists():
            vectors = _load_vector_manifest(settings)
        else:
            vectors = embed_documents(settings)
        result["vectors"] = {
            "total_records": vectors["total_records"],
            "total_bytes": vectors["total_bytes"],
            "passage_count": vectors["passage_count"],
            "shards": vectors["shards"],
        }
    if target != "documents":
        if settings.index_dir is not None and settings.index_dir.exists():
            result["index"] = _reuse_built_index(settings, target)
        else:
            result["index"] = build_index(settings, target)
    return result


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Convert local files and build a yappod2 index."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    def add_content_options(command_parser: argparse.ArgumentParser) -> None:
        content_group = command_parser.add_mutually_exclusive_group()
        content_group.add_argument(
            "--content-match",
            dest="content_match",
            action="store_true",
            help="enable formatter content_regex",
        )
        content_group.add_argument(
            "--no-content-match",
            dest="content_match",
            action="store_false",
            help="disable formatter content_regex",
        )
        command_parser.set_defaults(content_match=None)

    convert_parser = subparsers.add_parser("convert", help="extract local files into document shards")
    convert_parser.add_argument("--config", type=Path, required=True, help="local-files TOML config")
    add_content_options(convert_parser)

    build_parser = subparsers.add_parser(
        "build", help="build one index from existing pipeline shards"
    )
    build_parser.add_argument("--config", type=Path, required=True, help="local-files TOML config")
    build_parser.add_argument(
        "--target", choices=("lexical", "rag", "hybrid"), required=True
    )

    prepare_parser = subparsers.add_parser(
        "prepare", help="prepare passage shards from document shards"
    )
    prepare_parser.add_argument("--config", type=Path, required=True, help="local-files TOML config")

    embed_parser = subparsers.add_parser(
        "embed", help="attach passage embeddings to document shards"
    )
    embed_parser.add_argument("--config", type=Path, required=True, help="local-files TOML config")

    all_parser = subparsers.add_parser(
        "all", help="run every required stage for the selected target"
    )
    all_parser.add_argument("--config", type=Path, required=True, help="local-files TOML config")
    all_parser.add_argument(
        "--target",
        choices=("documents", "lexical", "rag", "hybrid"),
        required=True,
        help="final artifact to produce",
    )
    add_content_options(all_parser)
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "convert":
            settings = load_settings(args.config, args.content_match)
            result = convert(settings)
        elif args.command == "build":
            settings = load_settings(args.config)
            result = build_index(settings, args.target)
        elif args.command == "prepare":
            settings = load_settings(args.config)
            result = prepare_passages(settings)
        elif args.command == "embed":
            settings = load_settings(args.config)
            result = embed_documents(settings)
        elif args.command == "all":
            settings = load_settings(args.config, args.content_match)
            result = run_all(settings, args.target)
        else:
            raise LocalFilesError(f"unsupported command: {args.command}")
    except LocalFilesError as error:
        print(f"local-files: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, ensure_ascii=False, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
