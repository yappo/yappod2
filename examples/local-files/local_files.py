#!/usr/bin/env python3
"""Convert local files into sharded yappod2 canonical NDJSON."""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
from html.parser import HTMLParser
import json
import mimetypes
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from typing import Any, Dict, Iterator, List, Mapping, Optional, Sequence, Tuple
import unicodedata
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


def _bool(value: Any, name: str, default: bool) -> bool:
    if value is None:
        return default
    if not isinstance(value, bool):
        raise LocalFilesError(f"{name} must be true or false")
    return value


def _resolve(config_dir: Path, value: Any, name: str) -> Path:
    if not isinstance(value, str) or not value:
        raise LocalFilesError(f"{name} must be a non-empty path string")
    path = Path(value).expanduser()
    if not path.is_absolute():
        path = config_dir / path
    return path.resolve(strict=False)


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
    fingerprint_input = raw
    if content_match_override is not None:
        fingerprint_input += (
            b"\0cli.content_match=true" if content_match_override else b"\0cli.content_match=false"
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
            try:
                source_hash = sha256_file(path)
                text, extractor = extract_file(path, relative_path, settings)
                if not text.strip():
                    raise FileFailure("no_text", "extractor returned no searchable text", extractor)
                parts = split_body(text, settings.body_max_bytes)
                if not parts:
                    raise FileFailure("no_text", "extractor returned no searchable text", extractor)
                stat = path.stat()
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
                            "updated_at_unix_ms": max(0, stat.st_mtime_ns // 1_000_000),
                        }
                    )
                successful_files += 1
            except FileFailure as error:
                failures.write(_failure(relative_path, error))
            except OSError as error:
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


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Convert local files into sharded yappod2 canonical NDJSON."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    convert_parser = subparsers.add_parser("convert", help="extract local files into document shards")
    convert_parser.add_argument("--config", type=Path, required=True, help="local-files TOML config")
    content_group = convert_parser.add_mutually_exclusive_group()
    content_group.add_argument(
        "--content-match", dest="content_match", action="store_true", help="enable formatter content_regex"
    )
    content_group.add_argument(
        "--no-content-match",
        dest="content_match",
        action="store_false",
        help="disable formatter content_regex",
    )
    convert_parser.set_defaults(content_match=None)
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "convert":
            settings = load_settings(args.config, args.content_match)
            result = convert(settings)
        else:
            raise LocalFilesError(f"unsupported command: {args.command}")
    except LocalFilesError as error:
        print(f"local-files: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, ensure_ascii=False, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
