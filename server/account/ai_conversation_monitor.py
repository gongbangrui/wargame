"""Read-only, redacted access to the game server's AI planning history."""

from __future__ import annotations

import base64
import binascii
import hashlib
import re
from dataclasses import dataclass
from datetime import date
from importlib import import_module
from pathlib import Path
from typing import TYPE_CHECKING, Final, assert_never, override

from pydantic import ValidationError

if TYPE_CHECKING:
    from . import ai_conversation_monitor_models as models
else:
    _MODELS_MODULE = (
        f"{__package__}.ai_conversation_monitor_models"
        if __package__
        else "ai_conversation_monitor_models"
    )
    models = import_module(_MODELS_MODULE)

MAX_LIMIT: Final = 100
_CURSOR_VERSION: Final = 1
_MAX_SOURCE_BYTES: Final = 10 * 1024 * 1024
_DAILY_FILE_PATTERN = re.compile(
    r"^ai-conversations-(?P<date>[0-9]{4}-[0-9]{2}-[0-9]{2})(?:-[0-9]+)?\.jsonl$"
)


@dataclass(frozen=True, slots=True)
class InvalidMonitorCursor(ValueError):
    @override
    def __str__(self) -> str:
        return "invalid monitor cursor"


@dataclass(frozen=True, slots=True)
class InvalidConversationId(ValueError):
    @override
    def __str__(self) -> str:
        return "invalid conversation identifier"


@dataclass(frozen=True, slots=True)
class InvalidConversationStatus(ValueError):
    status: str

    @override
    def __str__(self) -> str:
        return f"invalid conversation status: {self.status}"


@dataclass(frozen=True, slots=True)
class _Entry:
    record: models.ConversationRecord
    sort_key: tuple[str, str, int]


@dataclass(frozen=True, slots=True)
class ConversationQuery:
    status: str
    limit: int
    before: str | None


def _history_files(root: Path) -> tuple[Path, ...]:
    try:
        if root.is_symlink():
            return ()
        resolved_root = root.resolve(strict=True)
        if not resolved_root.is_dir():
            return ()
        files: list[tuple[Path, int]] = []
        for candidate in resolved_root.iterdir():
            matched = _DAILY_FILE_PATTERN.fullmatch(candidate.name)
            if matched is None:
                continue
            try:
                _ = date.fromisoformat(matched.group("date"))
                size = candidate.stat().st_size
                resolved_candidate = candidate.resolve(strict=True)
            except (OSError, ValueError):
                continue
            if (
                candidate.is_symlink()
                or not resolved_candidate.is_file()
                or resolved_candidate.parent != resolved_root
                or size > _MAX_SOURCE_BYTES
            ):
                continue
            files.append((resolved_candidate, size))
        selected: list[Path] = []
        selected_bytes = 0
        for candidate, size in sorted(
            files, key=lambda item: item[0].name, reverse=True
        ):
            if selected_bytes + size > _MAX_SOURCE_BYTES:
                break
            selected.append(candidate)
            selected_bytes += size
        return tuple(selected)
    except OSError:
        return ()


def _entries(root: Path) -> tuple[_Entry, ...]:
    parsed: list[_Entry] = []
    for source in _history_files(root):
        source_digest = hashlib.sha256(source.name.encode("utf-8")).hexdigest()[:32]
        try:
            bytes_read = 0
            with source.open("rb") as handle:
                for line_number, line in enumerate(handle, start=1):
                    bytes_read += len(line)
                    if bytes_read > _MAX_SOURCE_BYTES:
                        break
                    record = models.parse_line(line.decode("utf-8", errors="replace"))
                    if record is not None:
                        parsed.append(
                            _Entry(
                                record,
                                (
                                    models.canonical_time(record.timestamp),
                                    source_digest,
                                    line_number,
                                ),
                            )
                        )
        except OSError:
            continue
    parsed.sort(key=lambda item: item.sort_key, reverse=True)
    unique: dict[str, _Entry] = {}
    for item in parsed:
        _ = unique.setdefault(item.record.conversation_id, item)
    return tuple(unique.values())


def _cursor_key(cursor: str) -> tuple[str, str, int]:
    if not cursor or len(cursor) > 512:
        raise InvalidMonitorCursor()
    try:
        encoded = cursor.encode("ascii")
        padding = b"=" * (-len(encoded) % 4)
        decoded = base64.b64decode(encoded + padding, altchars=b"-_", validate=True)
        payload = models.CursorPayload.model_validate_json(decoded)
    except (UnicodeEncodeError, binascii.Error, ValidationError):
        raise InvalidMonitorCursor() from None
    return payload.t, payload.f, payload.n


def _make_cursor(key: tuple[str, str, int]) -> str:
    payload = models.CursorPayload(v=_CURSOR_VERSION, t=key[0], f=key[1], n=key[2])
    return (
        base64.urlsafe_b64encode(payload.model_dump_json().encode("utf-8"))
        .decode("ascii")
        .rstrip("=")
    )


def _summary(entry: _Entry) -> models.ConversationSummary:
    record = entry.record
    execution_engine = ""
    fallback_engine = ""
    match record.execution_result:
        case dict() as result:
            engine = result.get("engine")
            execution_engine = engine if isinstance(engine, str) else ""
        case None | bool() | int() | float() | str() | list():
            pass
        case unreachable:
            assert_never(unreachable)
    match record.fallback:
        case dict() as fallback:
            engine = fallback.get("engine")
            fallback_engine = engine if isinstance(engine, str) else ""
        case None | bool() | int() | float() | str() | list():
            pass
        case unreachable:
            assert_never(unreachable)
    return models.ConversationSummary(
        conversation_id=record.conversation_id,
        request_id=models.redact_text(record.request_id or ""),
        room_id=models.redact_text(record.room_id or ""),
        planning_generation=models.positive_int(record.generation),
        timestamp=models.canonical_time(record.timestamp),
        configured_model=models.redact_text(record.configured_model or ""),
        resolved_model=models.redact_text(record.resolved_model or ""),
        status=record.status,
        failure_category=models.text_value(record.failure),
        latency_ms=models.positive_int(record.latency_ms),
        final_engine=models.redact_text(
            record.final_engine or execution_engine or fallback_engine
        ),
        rules_fallback_reason=models.text_value(record.fallback),
    )


def _message_from_history(
    messages: list[models.JsonValue], role: str
) -> models.JsonValue | None:
    for message in messages:
        if isinstance(message, dict) and message.get("role") == role:
            return message.get("content")
    return None


def _detail(entry: _Entry) -> models.ConversationDetail:
    record = entry.record
    system_message = record.system_message
    user_message = record.user_message
    if system_message is None:
        system_message = _message_from_history(record.messages, "system")
    if user_message is None:
        user_message = _message_from_history(record.messages, "user")
    summary = _summary(entry)
    return models.ConversationDetail(
        conversation_id=summary.conversation_id,
        request_id=summary.request_id,
        room_id=summary.room_id,
        planning_generation=summary.planning_generation,
        timestamp=summary.timestamp,
        configured_model=summary.configured_model,
        resolved_model=summary.resolved_model,
        status=summary.status,
        failure_category=summary.failure_category,
        latency_ms=summary.latency_ms,
        final_engine=summary.final_engine,
        rules_fallback_reason=summary.rules_fallback_reason,
        messages=[models.redact_json(message) for message in record.messages],
        system_message=models.redact_json(system_message),
        user_message=models.redact_json(user_message),
        raw_assistant_content=models.redact_json(record.raw_assistant),
        parsed_plan=models.redact_json(record.parsed_plan),
        execution_result=models.redact_json(record.execution_result),
        fallback=models.redact_json(record.fallback),
    )


def _status_filter(status: str) -> models.ConversationStatus | None:
    normalized = status.strip().casefold()
    if normalized in {"", "all"}:
        return None
    try:
        return models.ConversationStatus(normalized)
    except ValueError:
        raise InvalidConversationStatus(status=normalized) from None


def list_conversations(root: Path, query: ConversationQuery) -> models.ConversationPage:
    selected_status = _status_filter(query.status)
    bounded_limit = max(1, min(query.limit, MAX_LIMIT))
    records = tuple(
        item
        for item in _entries(root)
        if selected_status is None or item.record.status is selected_status
    )
    if query.before is not None:
        key = _cursor_key(query.before)
        records = tuple(item for item in records if item.sort_key < key)
    page = records[:bounded_limit]
    has_more = len(records) > bounded_limit
    return models.ConversationPage(
        conversations=[_summary(item) for item in page],
        next_before=_make_cursor(page[-1].sort_key) if has_more else None,
        has_more=has_more,
        limit=bounded_limit,
    )


def get_conversation(
    root: Path, conversation_id: str
) -> models.ConversationDetailResponse | None:
    """Return one redacted detail record, or ``None`` when it is absent."""
    if not models.valid_conversation_id(conversation_id):
        raise InvalidConversationId()
    for entry in _entries(root):
        if entry.record.conversation_id == conversation_id:
            return models.ConversationDetailResponse(conversation=_detail(entry))
    return None
