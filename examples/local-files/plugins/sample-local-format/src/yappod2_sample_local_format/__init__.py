"""Example MarkItDown plugin for files ending in .mydoc."""

from typing import Any, BinaryIO

from markitdown import DocumentConverter, DocumentConverterResult, MarkItDown, StreamInfo


__plugin_interface_version__ = 1


class MyDocumentConverter(DocumentConverter):
    def accepts(self, file_stream: BinaryIO, stream_info: StreamInfo, **kwargs: Any) -> bool:
        del file_stream, kwargs
        extension = (stream_info.extension or "").lower()
        return extension in {"mydoc", ".mydoc"}

    def convert(
        self, file_stream: BinaryIO, stream_info: StreamInfo, **kwargs: Any
    ) -> DocumentConverterResult:
        del stream_info, kwargs
        text = file_stream.read().decode("utf-8")
        if text.startswith("MYDOC\n"):
            text = text[len("MYDOC\n") :]
        return DocumentConverterResult(markdown=text)


def register_converters(markitdown: MarkItDown, **kwargs: Any) -> None:
    del kwargs
    markitdown.register_converter(MyDocumentConverter())
