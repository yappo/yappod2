#!/usr/bin/env python3
"""Download Japanese Wikipedia samples and produce yappod canonical NDJSON."""

import argparse
import bz2
import contextlib
import gzip
import hashlib
import json
import math
import os
from pathlib import Path
import re
import sys
import tempfile
import time
from typing import Dict, Iterable, Iterator, List, Optional, TextIO, Tuple
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen


DEFAULT_API_URL = "https://ja.wikipedia.org/w/api.php"
DEFAULT_DUMP_BASE_URL = "https://dumps.wikimedia.org/jawiki/latest"
DUMP_FILENAME = "jawiki-latest-pages-articles-multistream.xml.bz2"
CHECKSUM_FILENAME = "jawiki-latest-sha1sums.txt"
DUMP_CHECKSUM_PATTERN = re.compile(r"^jawiki-\d{8}-pages-articles-multistream\.xml\.bz2$")
DEFAULT_USER_AGENT = "yappod2-wikipedia-example/1.0 (https://github.com/yappo/yappod2)"
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
    base_url: str,
    model: str,
    inputs: List[str],
    dimensions: int,
    timeout: float,
    api_key: Optional[str],
) -> List[List[float]]:
    endpoint = base_url.rstrip("/") + ("/embeddings" if provider == "lmstudio" else "/api/embed")
    response = _post_json(endpoint, {"model": model, "input": inputs}, timeout, api_key)
    if provider == "ollama":
        embeddings = response.get("embeddings")
        if not isinstance(embeddings, list) or len(embeddings) != len(inputs):
            raise WikipediaDataError("Ollama embedding count does not match the input count")
        return [_validated_vector(item, dimensions) for item in embeddings]
    data = response.get("data")
    if not isinstance(data, list) or len(data) != len(inputs):
        raise WikipediaDataError("LM Studio embedding count does not match the input count")
    ordered: List[Optional[List[float]]] = [None] * len(inputs)
    for item in data:
        if not isinstance(item, dict):
            raise WikipediaDataError("LM Studio embedding item must be an object")
        index = item.get("index")
        if not isinstance(index, int) or isinstance(index, bool) or index < 0 or index >= len(inputs) or ordered[index] is not None:
            raise WikipediaDataError("LM Studio embedding indexes are invalid")
        ordered[index] = _validated_vector(item.get("embedding"), dimensions)
    if any(item is None for item in ordered):
        raise WikipediaDataError("LM Studio embedding response has missing indexes")
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
    base_url: str,
    model: str,
    dimensions: int,
    batch_size: int,
    timeout: float,
    profile: str,
    api_key: Optional[str] = None,
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
            raise WikipediaDataError("passage text must be non-empty")
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
            provider, base_url, model, texts[offset:offset + batch_size], dimensions, timeout, api_key
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


def fetch_api_documents(
    api_url: str,
    topics: Iterable[str],
    limit: int,
    output_path: Path,
    user_agent: str,
) -> Tuple[int, int]:
    seen = set()
    written = 0
    skipped = 0
    with _atomic_text_output(output_path) as output:
        for topic in topics:
            if written >= limit:
                break
            parameters = {
                "action": "query",
                "format": "json",
                "formatversion": "2",
                "generator": "search",
                "gsrsearch": topic,
                "gsrnamespace": "0",
                "gsrlimit": str(min(50, limit - written)),
                "prop": "extracts|info",
                "exintro": "1",
                "explaintext": "1",
                "exsectionformat": "plain",
                "inprop": "url",
                "redirects": "1",
            }
            separator = "&" if "?" in api_url else "?"
            response = _request_json(api_url + separator + urlencode(parameters), user_agent)
            query = response.get("query", {})
            pages = query.get("pages", []) if isinstance(query, dict) else []
            if not isinstance(pages, list):
                raise WikipediaDataError("Wikimedia API pages must be an array")
            for page in pages:
                if written >= limit:
                    break
                if not isinstance(page, dict):
                    raise WikipediaDataError("Wikimedia API page must be an object")
                page_id = page.get("pageid")
                key = str(page_id)
                if key in seen:
                    skipped += 1
                    continue
                document = _canonical_document(
                    page_id,
                    page.get("title"),
                    page.get("extract"),
                    page.get("fullurl"),
                    page.get("lastrevid"),
                )
                seen.add(key)
                if document is None:
                    skipped += 1
                    continue
                _write_document(output, document)
                written += 1
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


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    api = subparsers.add_parser("fetch-api", help="fetch a small topic-balanced API sample")
    api.add_argument("--api-url", default=DEFAULT_API_URL)
    api.add_argument("--topic", action="append", dest="topics")
    api.add_argument("--limit", type=_positive_int, default=1000)
    api.add_argument("--output", type=Path, required=True)
    api.add_argument("--user-agent", default=os.environ.get("WIKIMEDIA_USER_AGENT", DEFAULT_USER_AGENT))

    download = subparsers.add_parser("download-dump", help="download and verify the official full XML dump")
    download.add_argument("--base-url", default=DEFAULT_DUMP_BASE_URL)
    download.add_argument("--output-dir", type=Path, required=True)
    download.add_argument("--user-agent", default=os.environ.get("WIKIMEDIA_USER_AGENT", DEFAULT_USER_AGENT))

    convert = subparsers.add_parser("convert-dump", help="convert WikiExtractor JSON output")
    convert.add_argument("--input", type=Path, required=True)
    convert.add_argument("--output", type=Path, required=True)
    convert.add_argument("--limit", type=_positive_int)

    embed = subparsers.add_parser("embed", help="attach LM Studio or Ollama embeddings to canonical documents")
    embed.add_argument("--documents", type=Path, required=True)
    embed.add_argument("--passages", type=Path, required=True)
    embed.add_argument("--output", type=Path, required=True)
    embed.add_argument("--provider", choices=("lmstudio", "ollama"), required=True)
    embed.add_argument("--base-url", required=True)
    embed.add_argument("--model", required=True)
    embed.add_argument("--dimensions", type=_positive_int, default=768)
    embed.add_argument("--batch-size", type=_positive_int, default=16)
    embed.add_argument("--timeout", type=float, default=60.0)
    embed.add_argument("--api-key", default=os.environ.get("EMBEDDING_API_KEY"))
    embed.add_argument("--profile", choices=("embeddinggemma", "plain"), default="embeddinggemma")
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
            if args.timeout <= 0:
                raise WikipediaDataError("embedding timeout must be greater than zero")
            documents, passages = embed_documents(
                args.documents, args.passages, args.output, args.provider, args.base_url,
                args.model, args.dimensions, args.batch_size, args.timeout, args.profile, args.api_key,
            )
            result = {"output": str(args.output), "documents": documents, "passages": passages}
    except WikipediaDataError as error:
        print("error: {}".format(error), file=sys.stderr)
        return 1
    print(json.dumps(result, ensure_ascii=False, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
