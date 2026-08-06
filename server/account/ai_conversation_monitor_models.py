"""Typed boundary models and redaction helpers for AI history records."""

from __future__ import annotations

import json
import re
from datetime import datetime, timezone
from enum import StrEnum
from typing import ClassVar, Final, Literal, TypeAlias, assert_never

from pydantic import (
    AliasChoices,
    BaseModel,
    ConfigDict,
    Field,
    TypeAdapter,
    ValidationError,
)
from pydantic import (
    JsonValue as PydanticJsonValue,
)

JsonValue: TypeAlias = PydanticJsonValue
JsonObject: TypeAlias = dict[str, JsonValue]


class ConversationStatus(StrEnum):
    COMPLETED = "completed"
    REJECTED = "rejected"
    FAILED = "failed"
    CANCELLED = "cancelled"


_JSON_ADAPTER: Final[TypeAdapter[JsonValue]] = TypeAdapter(JsonValue)
_REDACTED: Final = "[REDACTED]"
_SENSITIVE_KEY_PATTERN = re.compile(
    r"password|passwd|passphrase|token|authorization|authheader|cookie|secret|"
    + r"apikey|credential|privatekey|internalkey|bearer"
)
_ID_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}$")
_BEARER_PATTERN = re.compile(r"\bBearer\s+[^\s,;]+", re.IGNORECASE)
_ASSIGNMENT_PATTERN = re.compile(
    r"\b(authorization|set-cookie|cookie|x-api-key|api[_-]?key|password|passwd|"
    + r"passphrase|token|secret|credential|private[_-]?key|internal[_-]?key)"
    + r"\s*([:=])\s*(?:\"[^\"]*\"|'[^']*'|[^\s,;]+)",
    re.IGNORECASE,
)


class ConversationRecord(BaseModel):
    """Typed allowlisted shape for one untrusted JSONL record."""

    model_config: ClassVar[ConfigDict] = ConfigDict(extra="ignore", frozen=True)

    conversation_id: str = Field(
        max_length=128,
        validation_alias=AliasChoices(
            "conversationId", "conversation_id", "conversation"
        ),
    )
    request_id: str | None = Field(
        default=None,
        max_length=256,
        validation_alias=AliasChoices("requestId", "request_id"),
    )
    room_id: str | None = Field(
        default=None,
        max_length=128,
        validation_alias=AliasChoices("roomId", "room", "room_id"),
    )
    generation: int | str | None = Field(
        default=None,
        validation_alias=AliasChoices(
            "planningGeneration", "generation", "planning_generation"
        ),
    )
    timestamp: str = Field(
        max_length=128,
        validation_alias=AliasChoices("timestamp", "time", "createdAt", "created_at"),
    )
    status: ConversationStatus
    failure: JsonValue | None = Field(
        default=None,
        validation_alias=AliasChoices("failureCategory", "failure", "failureClass"),
    )
    latency_ms: int | str | None = Field(
        default=None, validation_alias=AliasChoices("latencyMs", "latency_ms")
    )
    configured_model: str | None = Field(
        default=None,
        max_length=256,
        validation_alias=AliasChoices("configuredModel", "configured_model"),
    )
    resolved_model: str | None = Field(
        default=None,
        max_length=256,
        validation_alias=AliasChoices("resolvedModel", "model", "resolved_model"),
    )
    messages: list[JsonValue] = Field(default_factory=list, max_length=64)
    system_message: JsonValue | None = Field(
        default=None,
        validation_alias=AliasChoices(
            "systemMessage", "systemPrompt", "system_message"
        ),
    )
    user_message: JsonValue | None = Field(
        default=None,
        validation_alias=AliasChoices("userMessage", "prompt", "user_message"),
    )
    raw_assistant: JsonValue | None = Field(
        default=None,
        validation_alias=AliasChoices(
            "rawAssistantContent", "rawAssistant", "raw", "assistantResponse"
        ),
    )
    parsed_plan: JsonValue | None = Field(
        default=None, validation_alias=AliasChoices("parsedPlan", "parsed", "plan")
    )
    execution_result: JsonValue | None = Field(
        default=None,
        validation_alias=AliasChoices(
            "executionResult", "execution", "final", "finalResult"
        ),
    )
    final_engine: str | None = Field(
        default=None, validation_alias=AliasChoices("finalEngine", "engine")
    )
    fallback: JsonValue | None = Field(
        default=None,
        validation_alias=AliasChoices(
            "rulesFallbackReason", "fallback", "fallbackReason"
        ),
    )


class MonitorResponseModel(BaseModel):
    model_config: ClassVar[ConfigDict] = ConfigDict(
        extra="forbid", frozen=True, populate_by_name=True
    )


class ConversationSummary(MonitorResponseModel):
    conversation_id: str = Field(alias="conversationId")
    request_id: str = Field(alias="requestId")
    room_id: str = Field(alias="roomId")
    planning_generation: int = Field(alias="planningGeneration", ge=0)
    timestamp: str
    configured_model: str = Field(alias="configuredModel")
    resolved_model: str = Field(alias="resolvedModel")
    status: ConversationStatus
    failure_category: str = Field(alias="failureCategory")
    latency_ms: int = Field(alias="latencyMs", ge=0)
    final_engine: str = Field(alias="finalEngine")
    rules_fallback_reason: str = Field(alias="rulesFallbackReason")


class ConversationDetail(ConversationSummary):
    messages: list[JsonValue]
    system_message: JsonValue = Field(alias="systemMessage")
    user_message: JsonValue = Field(alias="userMessage")
    raw_assistant_content: JsonValue = Field(alias="rawAssistantContent")
    parsed_plan: JsonValue = Field(alias="parsedPlan")
    execution_result: JsonValue = Field(alias="executionResult")
    fallback: JsonValue


class ConversationPage(MonitorResponseModel):
    conversations: list[ConversationSummary]
    next_before: str | None = Field(alias="nextBefore")
    has_more: bool = Field(alias="hasMore")
    limit: int = Field(ge=1, le=100)


class ConversationDetailResponse(MonitorResponseModel):
    conversation: ConversationDetail


class CursorPayload(BaseModel):
    model_config: ClassVar[ConfigDict] = ConfigDict(extra="forbid", frozen=True)

    v: Literal[1]
    t: str = Field(min_length=1, max_length=128)
    f: str = Field(pattern=r"^[0-9a-f]{32}$")
    n: int = Field(ge=1, le=10_000_000)


def _is_sensitive_key(value: str) -> bool:
    normalized = "".join(
        character for character in value.casefold() if character.isalnum()
    )
    return _SENSITIVE_KEY_PATTERN.search(normalized) is not None


def redact_text(value: str) -> str:
    try:
        parsed = _JSON_ADAPTER.validate_json(value)
    except ValidationError:
        parsed = None
    match parsed:
        case dict() | list():
            return json.dumps(
                redact_json(parsed), ensure_ascii=False, separators=(",", ":")
            )
        case None | bool() | int() | float() | str():
            redacted = _BEARER_PATTERN.sub("Bearer [REDACTED]", value)
            return _ASSIGNMENT_PATTERN.sub(
                lambda match: f"{match.group(1)}{match.group(2)}{_REDACTED}",
                redacted,
            )
        case unreachable:
            assert_never(unreachable)


def redact_json(value: JsonValue) -> JsonValue:
    """Recursively redact sensitive keys in parsed JSON data."""
    match value:
        case str() as text:
            return redact_text(text)
        case None | bool() | int() | float():
            return value
        case list() as values:
            return [redact_json(item) for item in values]
        case dict() as values:
            return {
                key: _REDACTED if _is_sensitive_key(key) else redact_json(item)
                for key, item in values.items()
            }
        case unreachable:
            assert_never(unreachable)


def text_value(value: JsonValue | None) -> str:
    match value:
        case str() as text:
            return redact_text(text[:4096])
        case dict() as values:
            for key in ("category", "code", "reason", "failureClass", "message"):
                candidate = values.get(key)
                if isinstance(candidate, str):
                    return redact_text(candidate[:4096])
            return ""
        case None | bool() | int() | float() | list():
            return ""
        case unreachable:
            assert_never(unreachable)


def positive_int(value: int | str | None) -> int:
    match value:
        case bool() | None:
            return 0
        case int() as number:
            return min(max(number, 0), 2**63 - 1)
        case str() as text if text.isdecimal():
            return min(int(text), 2**63 - 1)
        case str():
            return 0
        case unreachable:
            assert_never(unreachable)


def canonical_time(value: str | None) -> str:
    if not isinstance(value, str) or len(value) > 128:
        return ""
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return ""
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return (
        parsed.astimezone(timezone.utc)
        .isoformat(timespec="milliseconds")
        .replace("+00:00", "Z")
    )


def parse_line(line: str) -> ConversationRecord | None:
    try:
        record = ConversationRecord.model_validate_json(line)
    except ValidationError:
        return None
    if _ID_PATTERN.fullmatch(record.conversation_id) is None or not canonical_time(
        record.timestamp
    ):
        return None
    return record


def valid_conversation_id(value: str) -> bool:
    return _ID_PATTERN.fullmatch(value) is not None
