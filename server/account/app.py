from __future__ import annotations

import errno
import fcntl
import hashlib
import ipaddress
import json
import math
import os
import pty
import re
import secrets
import sqlite3
import subprocess
import sys
import termios
import threading
import time
import urllib.error
import urllib.request
from contextlib import asynccontextmanager, contextmanager
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import AsyncIterator, Final, Iterator, Literal
from urllib.parse import urlsplit

import anyio
from argon2 import PasswordHasher
from argon2.exceptions import VerificationError
from fastapi import Depends, FastAPI, Header, HTTPException, Query, Request, WebSocket, WebSocketDisconnect, status
from fastapi.responses import FileResponse
from pydantic import BaseModel, Field, field_validator, model_validator
from starlette.websockets import WebSocketState

_ACCOUNT_SERVICE_DIR = str(Path(__file__).resolve().parent)
if _ACCOUNT_SERVICE_DIR not in sys.path:
    sys.path.insert(0, _ACCOUNT_SERVICE_DIR)

from ai_conversation_monitor import (
    ConversationQuery,
    InvalidConversationId,
    InvalidConversationStatus,
    InvalidMonitorCursor,
    get_conversation,
    list_conversations,
)
from ai_conversation_monitor_models import ConversationDetailResponse, ConversationPage


class ConfigurationError(RuntimeError):
    pass


def parse_web_shell_allowed_origins(value: str) -> frozenset[str]:
    origins = frozenset(part.strip() for part in value.split(",") if part.strip())
    if not origins:
        raise ConfigurationError("WEB_SHELL_ALLOWED_ORIGINS must contain at least one origin")
    for origin in origins:
        parsed = urlsplit(origin)
        try:
            port = parsed.port
        except ValueError as exc:
            raise ConfigurationError("WEB_SHELL_ALLOWED_ORIGINS contains an invalid port") from exc
        host = (
            f"[{parsed.hostname}]"
            if parsed.hostname is not None and ":" in parsed.hostname
            else parsed.hostname
        )
        normalized = (
            f"{parsed.scheme}://{host}{f':{port}' if port is not None else ''}"
            if host is not None
            else ""
        )
        if (
            parsed.scheme not in {"http", "https"}
            or parsed.hostname is None
            or parsed.username is not None
            or parsed.password is not None
            or parsed.path
            or parsed.query
            or parsed.fragment
            or "*" in origin
            or origin != normalized
        ):
            raise ConfigurationError(
                "WEB_SHELL_ALLOWED_ORIGINS must use exact scheme://host[:port] origins"
            )
    return origins


def parse_trusted_proxy_networks(
    value: str,
) -> tuple[ipaddress.IPv4Network | ipaddress.IPv6Network, ...]:
    return tuple(
        ipaddress.ip_network(part.strip(), strict=False)
        for part in value.split(",")
        if part.strip()
    )


def normalize_ollama_host(value: str) -> str:
    candidate = value.strip()
    if candidate.startswith("[") and candidate.endswith("]"):
        candidate = candidate[1:-1]
    if not candidate or any(character in candidate for character in "\r\n/?#@"):
        raise ValueError("Ollama 地址无效")
    try:
        ipaddress.ip_address(candidate)
    except ValueError:
        if re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9.-]{0,252}[A-Za-z0-9])?", candidate) is None:
            raise ValueError("Ollama 地址必须是主机名或 IP 地址")
    return candidate


def ollama_base_url_parts(value: str) -> tuple[str, str, int]:
    parsed = urlsplit(value.strip())
    try:
        port = parsed.port
    except ValueError as exc:
        raise ValueError("Ollama 地址端口无效") from exc
    if (
        parsed.scheme not in {"http", "https"}
        or parsed.hostname is None
        or port is None
        or parsed.username is not None
        or parsed.password is not None
        or parsed.path not in {"", "/"}
        or parsed.query
        or parsed.fragment
    ):
        raise ValueError("Ollama 地址必须是带端口的 http/https 地址")
    return parsed.scheme, normalize_ollama_host(parsed.hostname), port


def build_ollama_base_url(scheme: str, host: str, port: int) -> str:
    normalized_host = normalize_ollama_host(host)
    if scheme not in {"http", "https"} or not 1 <= port <= 65535:
        raise ValueError("Ollama 协议或端口无效")
    formatted_host = f"[{normalized_host}]" if ":" in normalized_host else normalized_host
    return f"{scheme}://{formatted_host}:{port}"


DATA_DIR = Path(os.getenv("DATA_DIR", "/data"))
DB_PATH = DATA_DIR / "wargame.db"
STATIC_DIR = Path(__file__).resolve().parent / "static"
SHELL_WORKING_DIR = Path("/app") if Path("/app").is_dir() else Path(__file__).resolve().parent
SESSION_HOURS = int(os.getenv("SESSION_HOURS", "12"))
WARGAME_VERSION = os.getenv("WARGAME_VERSION", "dev").strip() or "dev"
WARGAME_SOURCE_DIGEST = os.getenv("WARGAME_SOURCE_DIGEST", "dev").strip() or "dev"
INTERNAL_KEY = os.getenv("INTERNAL_API_KEY", "").strip()
PUBLIC_WS_URL = os.getenv("PUBLIC_GAME_WS_URL", "ws://localhost:8090")
GAME_STATUS_PATH = Path(os.getenv("GAME_STATUS_PATH", "/data/game-status.json"))
GAME_EVENTS_PATH = Path(os.getenv("GAME_EVENTS_PATH", "/data/game-events.jsonl"))
MONITOR_READY_WAIT_SECONDS = 1.25
ROOM_OPERATION_TIMEOUT_SECONDS = max(10, min(int(os.getenv("ROOM_OPERATION_TIMEOUT_SECONDS", "20")), 300))
WEB_SHELL_ENABLED = os.getenv("WEB_SHELL_ENABLED", "false").strip().lower() == "true"
WEB_SHELL_TICKET_SECONDS = max(30, min(int(os.getenv("WEB_SHELL_TICKET_SECONDS", "120")), 600))
WEB_SHELL_SESSION_SECONDS = max(60, min(int(os.getenv("WEB_SHELL_SESSION_SECONDS", "900")), 3600))
WEB_SHELL_MAX_SESSIONS = max(1, min(int(os.getenv("WEB_SHELL_MAX_SESSIONS", "2")), 8))
WEB_SHELL_ALLOWED_ORIGINS = parse_web_shell_allowed_origins(
    os.getenv(
        "WEB_SHELL_ALLOWED_ORIGINS",
        "http://127.0.0.1:8080,http://localhost:8080",
    )
)
LOGIN_TRUSTED_PROXY_NETWORKS = parse_trusted_proxy_networks(
    os.getenv("LOGIN_TRUSTED_PROXIES", "")
)
LOGIN_WINDOW_SECONDS = max(1, min(int(os.getenv("LOGIN_WINDOW_SECONDS", "60")), 60))
LOGIN_SUBJECT_IP_FAILURE_LIMIT = max(
    1, min(int(os.getenv("LOGIN_SUBJECT_IP_FAILURE_LIMIT", "5")), 5)
)
LOGIN_IP_FAILURE_LIMIT = max(1, min(int(os.getenv("LOGIN_IP_FAILURE_LIMIT", "20")), 20))
ACTIVE_GAME_ROOM_ID = os.getenv("GAME_ROOM_ID", "main").strip() or "main"
MIN_PASSWORD_LENGTH: Final = 8
# 联网账号角色与房间战位是两套独立身份。旧版本的 editor 角色保留为
# 兼容输入，但 API 和游戏会话统一使用 room_admin。
ROLES = {"player", "room_admin"}
SEAT_TYPES = {"commander", "attack", "recon", "ground", "jammer"}
SEAT_BASE_KEYS = {f"{side}_{seat_type}" for side in ("red", "blue") for seat_type in SEAT_TYPES}
DEFAULT_SEAT_LIMITS = {
    "red_commander": 1, "red_attack": 1, "red_recon": 1,
    "red_ground": 1, "red_jammer": 1,
    "blue_commander": 1, "blue_attack": 1, "blue_recon": 1,
    "blue_ground": 1, "blue_jammer": 1,
}
DEFAULT_OLLAMA_BASE_URL: Final = "http://host.docker.internal:11434"
DEFAULT_OLLAMA_MODEL: Final = "auto"
LEGACY_OLLAMA_MODEL: Final = "qwen3.5:4b"
OLLAMA_PROBE_TIMEOUT_SECONDS: Final = max(
    0.25, min(float(os.getenv("OLLAMA_PROBE_TIMEOUT_SECONDS", "2.0")), 10.0)
)
OLLAMA_PROBE_MAX_BYTES: Final = 1024 * 1024
_ollama_models_lock = threading.Lock()
_ollama_models_cache: dict[str, object] = {
    "baseUrl": "",
    "models": [],
    "connected": False,
    "checkedAt": "",
    "error": "",
}


def environment_ai_config() -> dict[str, str | int]:
    provider = os.getenv("AI_PROVIDER", "auto").strip().lower() or "auto"
    if provider not in {"rules", "auto", "ollama"}:
        raise ConfigurationError("AI_PROVIDER 必须是 rules、auto 或 ollama")
    try:
        scheme, host, port = ollama_base_url_parts(
            os.getenv("OLLAMA_BASE_URL", DEFAULT_OLLAMA_BASE_URL)
        )
    except ValueError as exc:
        raise ConfigurationError(str(exc)) from exc
    model = os.getenv("OLLAMA_MODEL", DEFAULT_OLLAMA_MODEL).strip()
    if not model or len(model) > 128 or any(character in model for character in "\r\n"):
        raise ConfigurationError("OLLAMA_MODEL 无效")
    return {
        "provider": provider,
        "ollamaScheme": scheme,
        "ollamaHost": host,
        "ollamaPort": port,
        "baseUrl": build_ollama_base_url(scheme, host, port),
        "model": model,
    }

password_hasher = PasswordHasher(time_cost=3, memory_cost=65536, parallelism=2)
terminal_tickets: dict[str, tuple[int, str, float]] = {}
terminal_tickets_lock = threading.Lock()
active_terminal_sessions = 0
active_terminal_sessions_lock = threading.Lock()


@asynccontextmanager
async def lifespan(_: FastAPI) -> AsyncIterator[None]:
    if len(INTERNAL_KEY) < 32 or INTERNAL_KEY == "change-this-internal-key":
        raise RuntimeError("INTERNAL_API_KEY 必须是至少 32 位的随机密钥")
    initialize_database()
    yield


app = FastAPI(
    title="兵棋推演控制台",
    docs_url=None,
    redoc_url=None,
    openapi_url=None,
    lifespan=lifespan,
)


class LoginBody(BaseModel):
    username: str = Field(min_length=1, max_length=64)
    password: str = Field(min_length=MIN_PASSWORD_LENGTH)

    @field_validator("username", mode="before")
    @classmethod
    def strip_username(cls, value: object) -> object:
        return value.strip() if isinstance(value, str) else value


class UserBody(BaseModel):
    username: str = Field(min_length=3, max_length=64, pattern=r"^[A-Za-z0-9_.-]+$")
    display_name: str = Field(min_length=1, max_length=64)
    role: Literal["player", "room_admin", "editor"] = "player"
    password: str | None = Field(default=None)
    enabled: bool = True

    @field_validator("username", "display_name", mode="before")
    @classmethod
    def strip_text(cls, value: object) -> object:
        return value.strip() if isinstance(value, str) else value

    @field_validator("password", mode="before")
    @classmethod
    def validate_optional_password(cls, value: object) -> object:
        if value == "":
            return None
        if isinstance(value, str) and len(value) < MIN_PASSWORD_LENGTH:
            raise ValueError(f"password must contain at least {MIN_PASSWORD_LENGTH} characters")
        return value

    @field_validator("role", mode="before")
    @classmethod
    def normalize_role_value(cls, value: object) -> object:
        if value == "editor":
            return "room_admin"
        return value


class PasswordBody(BaseModel):
    current_password: str = Field(min_length=MIN_PASSWORD_LENGTH)
    new_password: str = Field(min_length=MIN_PASSWORD_LENGTH)


class TokenBody(BaseModel):
    token: str = Field(min_length=20, max_length=256)


class TerminalLoginBody(BaseModel):
    password: str = Field(min_length=MIN_PASSWORD_LENGTH)


class RoomBody(BaseModel):
    room_id: str = Field(min_length=2, max_length=64, pattern=r"^[A-Za-z0-9_.-]+$")
    name: str = Field(min_length=1, max_length=96)
    description: str = Field(default="", max_length=512)
    scenario_id: str = Field(default="default", max_length=128)
    protocol_profile: Literal["native", "vmf-guided-strike-v1"] = "native"
    seat_limits: dict[str, int] = Field(default_factory=dict)
    seat_parameters: dict[str, dict[str, float | int | bool]] = Field(default_factory=dict)
    enabled: bool = True
    mode: Literal["pvp", "pve"] = "pvp"
    ai_difficulty: Literal["easy", "normal", "hard"] = "normal"
    # Optional on input for backwards-compatible clients.  The service fills
    # the values from the room/global defaults when a room is created, and
    # preserves the stored values when an existing room is updated.
    ai_provider: Literal["rules", "ollama"] | None = None
    ai_model: str | None = Field(default=None, max_length=128)
    intel_stale_after_sec: float = Field(default=10.0, gt=0.0, le=86400.0)
    intel_archive_after_sec: float = Field(default=120.0, gt=0.0, le=604800.0)

    @model_validator(mode="after")
    def validate_intel_windows(self) -> "RoomBody":
        if self.intel_archive_after_sec <= self.intel_stale_after_sec:
            raise ValueError("情报归档阈值必须大于失联阈值")
        return self

    @field_validator("room_id", "name", "description", "scenario_id", mode="before")
    @classmethod
    def strip_room_text(cls, value: object) -> object:
        return value.strip() if isinstance(value, str) else value

    @field_validator("ai_model", mode="before")
    @classmethod
    def strip_ai_model(cls, value: object) -> object:
        if value is None:
            return value
        return value.strip() if isinstance(value, str) else value

    @field_validator("ai_model")
    @classmethod
    def validate_ai_model(cls, value: str | None) -> str | None:
        if value is not None and any(character in value for character in "\r\n"):
            raise ValueError("模型名称不能包含换行")
        return value

    @field_validator("seat_limits")
    @classmethod
    def validate_seat_limits(cls, value: dict[str, int]) -> dict[str, int]:
        for key, count in value.items():
            if key not in SEAT_BASE_KEYS:
                raise ValueError("不支持的战位类型")
            if count < 0 or count > 64:
                raise ValueError("战位数量必须在 0 到 64 之间")
            if key.endswith("_commander") and count != 1:
                raise ValueError("红蓝双方指挥官数量必须固定为 1")
        return value


    @field_validator("seat_parameters")
    @classmethod
    def validate_seat_parameters(
        cls, value: dict[str, dict[str, float | int | bool]]
    ) -> dict[str, dict[str, float | int | bool]]:
        allowed_fields = {"communicationRange", "detectRange"}
        for key, parameters in value.items():
            parts = key.split("_")
            base_key = "_".join(parts[:2])
            if base_key not in SEAT_BASE_KEYS or len(parts) not in (2, 3) \
                    or (len(parts) == 3 and (not parts[2].isdigit() or int(parts[2]) <= 0)):
                raise ValueError("战位参数包含未知战位")
            unknown = set(parameters) - allowed_fields
            if unknown:
                raise ValueError(f"不支持的战位参数: {', '.join(sorted(unknown))}")
            for name, parameter in parameters.items():
                if isinstance(parameter, bool) or not isinstance(parameter, (int, float)):
                    raise ValueError(f"{name} 必须是数值")
                if parameter < 0 or parameter > 1_000_000:
                    raise ValueError(f"{name} 必须在 0 到 1000000 米之间")
        return value


class AiConfigBody(BaseModel):
    provider: Literal["rules", "auto", "ollama"] = "auto"
    ollama_scheme: Literal["http", "https"] = "http"
    ollama_host: str = Field(min_length=1, max_length=253)
    ollama_port: int = Field(ge=1, le=65535)
    model: str = Field(min_length=1, max_length=128)

    @field_validator("ollama_host", mode="before")
    @classmethod
    def strip_ollama_host(cls, value: object) -> object:
        if not isinstance(value, str):
            return value
        return normalize_ollama_host(value)

    @field_validator("model", mode="before")
    @classmethod
    def strip_model(cls, value: object) -> object:
        return value.strip() if isinstance(value, str) else value

    @field_validator("model")
    @classmethod
    def validate_model_name(cls, value: str) -> str:
        if any(character in value for character in "\r\n"):
            raise ValueError("模型名称不能包含换行")
        return value


class OllamaEndpointBody(BaseModel):
    ollama_scheme: Literal["http", "https"] = "http"
    ollama_host: str = Field(min_length=1, max_length=253)
    ollama_port: int = Field(ge=1, le=65535)
    # The endpoint API is also used by the console to retain the legacy
    # global defaults.  Both fields are optional so deployments which only
    # need to change the network endpoint remain source-compatible.
    provider: Literal["rules", "auto", "ollama"] | None = None
    model: str | None = Field(default=None, max_length=128)

    @field_validator("ollama_host", mode="before")
    @classmethod
    def strip_endpoint_host(cls, value: object) -> object:
        if not isinstance(value, str):
            return value
        return normalize_ollama_host(value)

    @field_validator("model", mode="before")
    @classmethod
    def strip_endpoint_model(cls, value: object) -> object:
        return value.strip() if isinstance(value, str) else value

    @field_validator("model")
    @classmethod
    def validate_endpoint_model(cls, value: str | None) -> str | None:
        if value is not None and any(character in value for character in "\r\n"):
            raise ValueError("模型名称不能包含换行")
        return value


class InternalRoomStatusBody(BaseModel):
    status: Literal["stopped", "preparing", "running", "paused", "finished"]
    reason: str = Field(default="", max_length=256)
    winner: Literal["", "red", "blue", "draw"] = ""
    occupants: list[dict[str, object]] = Field(default_factory=list, max_length=128)


class InternalOperationAckBody(BaseModel):
    state: Literal["acknowledged", "failed"]
    revision: int = Field(default=0, ge=0)
    code: str = Field(default="", max_length=128)
    ai_resolved_model: str = Field(default="", max_length=128)


class InternalPresenceBody(BaseModel):
    occupants: list[dict[str, object]] = Field(default_factory=list, max_length=128)


class InternalRoomConfigBody(BaseModel):
    expected_config_version: int = Field(ge=1)
    name: str = Field(min_length=1, max_length=96)
    description: str = Field(default="", max_length=512)
    scenario_id: str = Field(default="default", max_length=128)
    protocol_profile: Literal["native", "vmf-guided-strike-v1"] = "native"
    seat_limits: dict[str, int] = Field(default_factory=dict)
    seat_parameters: dict[str, dict[str, float | int | bool]] = Field(default_factory=dict)

    @field_validator("name", "description", "scenario_id", mode="before")
    @classmethod
    def strip_room_config_text(cls, value: object) -> object:
        return value.strip() if isinstance(value, str) else value

    @field_validator("seat_limits")
    @classmethod
    def validate_internal_seat_limits(cls, value: dict[str, int]) -> dict[str, int]:
        for key, count in value.items():
            if key not in SEAT_BASE_KEYS:
                raise ValueError("不支持的战位类型")
            if count < 0 or count > 64:
                raise ValueError("战位数量必须在 0 到 64 之间")
            if key.endswith("_commander") and count != 1:
                raise ValueError("红蓝双方指挥官数量必须固定为 1")
        return value

    @model_validator(mode="after")
    def validate_complete_internal_seat_limits(self) -> "InternalRoomConfigBody":
        # Omitted fields remain backwards compatible; a supplied capacity map
        # must be complete so the authority cannot silently drop seat types.
        if "seat_limits" in self.model_fields_set and set(self.seat_limits) != SEAT_BASE_KEYS:
            raise ValueError("战位容量必须包含红蓝双方完整配置")
        return self

    @field_validator("seat_parameters")
    @classmethod
    def validate_internal_seat_parameters(
        cls, value: dict[str, dict[str, float | int | bool]]
    ) -> dict[str, dict[str, float | int | bool]]:
        allowed_fields = {"communicationRange", "detectRange"}
        for key, parameters in value.items():
            parts = key.split("_")
            base_key = "_".join(parts[:2])
            if base_key not in SEAT_BASE_KEYS or len(parts) not in (2, 3) \
                    or (len(parts) == 3 and (not parts[2].isdigit() or int(parts[2]) <= 0)):
                raise ValueError("战位参数包含未知战位")
            unknown = set(parameters) - allowed_fields
            if unknown:
                raise ValueError(f"不支持的战位参数: {', '.join(sorted(unknown))}")
            for name, parameter in parameters.items():
                if isinstance(parameter, bool) or not isinstance(parameter, (int, float)):
                    raise ValueError(f"{name} 必须是数值")
                if parameter < 0 or parameter > 1_000_000:
                    raise ValueError(f"{name} 必须在 0 到 1000000 米之间")
        return value


class KickBody(BaseModel):
    reason: str = Field(default="管理员移出房间", max_length=256)


def utc_now() -> datetime:
    return datetime.now(timezone.utc)


def iso_time(value: datetime) -> str:
    return value.isoformat(timespec="seconds").replace("+00:00", "Z")


def token_digest(token: str) -> str:
    return hashlib.sha256(token.encode("utf-8")).hexdigest()


def normalize_user_role(value: object) -> str:
    return "room_admin" if str(value or "").strip().lower() in {"room_admin", "editor"} else "player"


def normalized_display_name(username: object, display_name: object) -> str:
    candidate = str(display_name or "").strip()
    return candidate or str(username or "").strip()


def queue_user_invalidation(
    db: sqlite3.Connection,
    user_id: int,
    reason: str,
    requested_by: int,
) -> None:
    pending = db.execute(
        "SELECT id FROM user_kick_requests WHERE user_id=? AND processed_at IS NULL LIMIT 1",
        (user_id,),
    ).fetchone()
    if pending is not None:
        return
    db.execute(
        "INSERT INTO user_kick_requests(user_id,reason,requested_by,requested_at) VALUES(?,?,?,?)",
        (user_id, reason, requested_by, iso_time(utc_now())),
    )


@contextmanager
def database() -> Iterator[sqlite3.Connection]:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    connection = sqlite3.connect(DB_PATH, timeout=10)
    connection.row_factory = sqlite3.Row
    connection.execute("PRAGMA foreign_keys = ON")
    try:
        yield connection
        connection.commit()
    finally:
        connection.close()


def initialize_database() -> None:
    with database() as db:
        db.executescript(
            """
            PRAGMA journal_mode = WAL;
            CREATE TABLE IF NOT EXISTS admins (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                username TEXT NOT NULL UNIQUE,
                password_hash TEXT NOT NULL,
                updated_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS users (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                username TEXT NOT NULL UNIQUE COLLATE NOCASE,
                display_name TEXT NOT NULL,
                role TEXT NOT NULL DEFAULT 'player' CHECK(role IN ('player','room_admin')),
                password_hash TEXT NOT NULL,
                enabled INTEGER NOT NULL DEFAULT 1,
                created_at TEXT NOT NULL,
                updated_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS sessions (
                token_hash TEXT PRIMARY KEY,
                subject_type TEXT NOT NULL CHECK(subject_type IN ('admin', 'player')),
                subject_id INTEGER NOT NULL,
                expires_at TEXT NOT NULL,
                created_at TEXT NOT NULL
            );
            CREATE INDEX IF NOT EXISTS idx_sessions_subject
                ON sessions(subject_type, subject_id);
            CREATE TABLE IF NOT EXISTS rooms (
                room_id TEXT PRIMARY KEY,
                name TEXT NOT NULL,
                description TEXT NOT NULL DEFAULT '',
                scenario_id TEXT NOT NULL DEFAULT 'default',
                protocol_profile TEXT NOT NULL DEFAULT 'native',
                seat_limits TEXT NOT NULL DEFAULT '{}',
                seat_parameters TEXT NOT NULL DEFAULT '{}',
                mode TEXT NOT NULL DEFAULT 'pvp',
                ai_difficulty TEXT NOT NULL DEFAULT 'normal',
                ai_provider TEXT NOT NULL DEFAULT 'rules',
                ai_model TEXT NOT NULL DEFAULT '',
                ai_resolved_model TEXT NOT NULL DEFAULT '',
                config_version INTEGER NOT NULL DEFAULT 1,
                intel_stale_after_sec REAL NOT NULL DEFAULT 10.0,
                intel_archive_after_sec REAL NOT NULL DEFAULT 120.0,
                status TEXT NOT NULL DEFAULT 'stopped',
                winner TEXT NOT NULL DEFAULT '',
                status_reason TEXT NOT NULL DEFAULT '',
                enabled INTEGER NOT NULL DEFAULT 1,
                created_at TEXT NOT NULL,
                updated_at TEXT NOT NULL
            );
            CREATE INDEX IF NOT EXISTS idx_rooms_enabled ON rooms(enabled, updated_at);
            CREATE TABLE IF NOT EXISTS ai_config (
                config_id INTEGER PRIMARY KEY CHECK(config_id = 1),
                provider TEXT NOT NULL,
                base_url TEXT NOT NULL,
                model TEXT NOT NULL,
                config_version INTEGER NOT NULL DEFAULT 1,
                updated_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS room_operations (
                operation_id TEXT PRIMARY KEY,
                room_id TEXT NOT NULL,
                action TEXT NOT NULL,
                expected_status TEXT NOT NULL,
                state TEXT NOT NULL CHECK(state IN ('pending', 'acknowledged', 'failed')),
                requested_revision INTEGER NOT NULL DEFAULT 0,
                requested_config_version INTEGER NOT NULL DEFAULT 1,
                requested_ollama_config_version INTEGER NOT NULL DEFAULT 1,
                applied_revision INTEGER,
                result_code TEXT NOT NULL DEFAULT '',
                requested_at TEXT NOT NULL,
                acknowledged_at TEXT,
                FOREIGN KEY(room_id) REFERENCES rooms(room_id)
            );
            CREATE INDEX IF NOT EXISTS idx_room_operations_latest
                ON room_operations(room_id, requested_at DESC);
            CREATE TABLE IF NOT EXISTS terminal_audit (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                admin_id INTEGER NOT NULL,
                action TEXT NOT NULL,
                created_at TEXT NOT NULL,
                FOREIGN KEY(admin_id) REFERENCES admins(id)
            );
            CREATE INDEX IF NOT EXISTS idx_terminal_audit_admin_time
                ON terminal_audit(admin_id, created_at);
            CREATE TABLE IF NOT EXISTS login_failures (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                subject_type TEXT NOT NULL,
                subject TEXT NOT NULL,
                ip_address TEXT NOT NULL,
                failed_at REAL NOT NULL
            );
            CREATE INDEX IF NOT EXISTS idx_login_failures_subject_ip_time
                ON login_failures(subject_type, subject, ip_address, failed_at);
            CREATE INDEX IF NOT EXISTS idx_login_failures_ip_time
                ON login_failures(ip_address, failed_at);
            CREATE TABLE IF NOT EXISTS room_presence (
                room_id TEXT NOT NULL,
                user_id INTEGER NOT NULL,
                username TEXT NOT NULL,
                display_name TEXT NOT NULL,
                seat_id TEXT NOT NULL DEFAULT '',
                seat_type TEXT NOT NULL DEFAULT '',
                side TEXT NOT NULL DEFAULT '',
                connected_at TEXT NOT NULL,
                last_seen_at TEXT NOT NULL,
                updated_at TEXT NOT NULL,
                PRIMARY KEY(room_id, user_id)
            );
            CREATE INDEX IF NOT EXISTS idx_room_presence_updated
                ON room_presence(room_id, updated_at);
            CREATE TABLE IF NOT EXISTS room_kick_requests (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                room_id TEXT NOT NULL,
                user_id INTEGER NOT NULL,
                reason TEXT NOT NULL DEFAULT '',
                requested_by INTEGER NOT NULL,
                requested_at TEXT NOT NULL,
                processed_at TEXT,
                FOREIGN KEY(requested_by) REFERENCES admins(id)
            );
            CREATE INDEX IF NOT EXISTS idx_room_kick_pending
                ON room_kick_requests(room_id, processed_at, requested_at);
            CREATE TABLE IF NOT EXISTS room_audit (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                room_id TEXT NOT NULL,
                user_id INTEGER NOT NULL,
                admin_id INTEGER NOT NULL,
                action TEXT NOT NULL,
                detail TEXT NOT NULL DEFAULT '',
                created_at TEXT NOT NULL,
                FOREIGN KEY(admin_id) REFERENCES admins(id)
            );
            CREATE INDEX IF NOT EXISTS idx_room_audit_time
                ON room_audit(room_id, created_at);
            CREATE TABLE IF NOT EXISTS user_kick_requests (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                user_id INTEGER NOT NULL,
                reason TEXT NOT NULL DEFAULT '',
                requested_by INTEGER NOT NULL,
                requested_at TEXT NOT NULL,
                processed_at TEXT,
                FOREIGN KEY(requested_by) REFERENCES admins(id)
            );
            CREATE INDEX IF NOT EXISTS idx_user_kick_pending
                ON user_kick_requests(processed_at, requested_at);
            """
        )
        room_columns = {row["name"] for row in db.execute("PRAGMA table_info(rooms)")}
        added_room_ai_columns: set[str] = set()
        for name, declaration in (
            ("winner", "TEXT NOT NULL DEFAULT ''"),
            ("status_reason", "TEXT NOT NULL DEFAULT ''"),
            ("mode", "TEXT NOT NULL DEFAULT 'pvp'"),
            ("ai_difficulty", "TEXT NOT NULL DEFAULT 'normal'"),
            ("ai_provider", "TEXT NOT NULL DEFAULT 'rules'"),
            ("ai_model", "TEXT NOT NULL DEFAULT ''"),
            ("ai_resolved_model", "TEXT NOT NULL DEFAULT ''"),
            ("config_version", "INTEGER NOT NULL DEFAULT 1"),
            ("intel_stale_after_sec", "REAL NOT NULL DEFAULT 10.0"),
            ("intel_archive_after_sec", "REAL NOT NULL DEFAULT 120.0"),
            ("protocol_profile", "TEXT NOT NULL DEFAULT 'native'"),
        ):
            if name not in room_columns:
                db.execute(f"ALTER TABLE rooms ADD COLUMN {name} {declaration}")
                added_room_ai_columns.add(name)
        db.execute(
            "UPDATE rooms SET mode='pvp' WHERE mode NOT IN ('pvp', 'pve') OR mode IS NULL"
        )
        db.execute(
            "UPDATE rooms SET protocol_profile='native' "
            "WHERE protocol_profile NOT IN ('native', 'vmf-guided-strike-v1') OR protocol_profile IS NULL"
        )
        db.execute(
            "UPDATE rooms SET ai_difficulty='normal' "
            "WHERE ai_difficulty NOT IN ('easy', 'normal', 'hard') OR ai_difficulty IS NULL"
        )
        db.execute("UPDATE rooms SET config_version=1 WHERE config_version IS NULL OR config_version < 1")
        db.execute(
            "UPDATE rooms SET intel_stale_after_sec=10.0 "
            "WHERE intel_stale_after_sec IS NULL OR intel_stale_after_sec <= 0"
        )
        db.execute(
            "UPDATE rooms SET intel_archive_after_sec=120.0 "
            "WHERE intel_archive_after_sec IS NULL OR intel_archive_after_sec <= intel_stale_after_sec"
        )
        db.execute(
            "UPDATE rooms SET ai_provider='rules' "
            "WHERE ai_provider NOT IN ('rules', 'ollama') OR ai_provider IS NULL"
        )
        db.execute("UPDATE rooms SET ai_model='' WHERE ai_model IS NULL")
        db.execute("UPDATE rooms SET ai_resolved_model='' WHERE ai_resolved_model IS NULL")
        operation_columns = {
            row["name"] for row in db.execute("PRAGMA table_info(room_operations)")
        }
        for name, declaration in (
            ("requested_revision", "INTEGER NOT NULL DEFAULT 0"),
            ("requested_config_version", "INTEGER NOT NULL DEFAULT 1"),
            ("requested_ollama_config_version", "INTEGER NOT NULL DEFAULT 1"),
            ("applied_revision", "INTEGER"),
            ("result_code", "TEXT NOT NULL DEFAULT ''"),
        ):
            if name not in operation_columns:
                db.execute(f"ALTER TABLE room_operations ADD COLUMN {name} {declaration}")
        terminal_audit_columns = {
            row["name"] for row in db.execute("PRAGMA table_info(terminal_audit)")
        }
        for name, declaration in (
            ("event", "TEXT NOT NULL DEFAULT ''"),
            ("outcome", "TEXT NOT NULL DEFAULT ''"),
        ):
            if name not in terminal_audit_columns:
                db.execute(f"ALTER TABLE terminal_audit ADD COLUMN {name} {declaration}")
        # Enforce the one-player/one-client invariant in the database as well
        # as in the request handler. Remove stale/legacy duplicate player
        # sessions before creating the partial unique index.
        now = iso_time(utc_now())
        db.execute("DELETE FROM sessions WHERE subject_type='player' AND expires_at <= ?", (now,))
        db.execute(
            "DELETE FROM sessions WHERE subject_type='player' AND rowid NOT IN ("
            "SELECT MAX(rowid) FROM sessions WHERE subject_type='player' GROUP BY subject_id)"
        )
        db.execute(
            "CREATE UNIQUE INDEX IF NOT EXISTS idx_single_player_session "
            "ON sessions(subject_type, subject_id) WHERE subject_type='player'"
        )
        db.execute(
            "UPDATE room_operations SET state='failed', acknowledged_at=? "
            "WHERE state='pending' AND rowid NOT IN ("
            "SELECT MAX(rowid) FROM room_operations WHERE state='pending' GROUP BY room_id)",
            (now,),
        )
        db.execute(
            "CREATE UNIQUE INDEX IF NOT EXISTS idx_single_pending_room_operation "
            "ON room_operations(room_id) WHERE state='pending'"
        )
        # 旧安装可能把 role 限制为红蓝席位。重建 users 表后保留账号、密码和
        # 时间戳，并把 editor 平滑迁移为 room_admin，其余历史席位迁移为 player。
        users_schema_row = db.execute(
            "SELECT sql FROM sqlite_master WHERE type='table' AND name='users'"
        ).fetchone()
        users_schema = (users_schema_row[0] if users_schema_row else "") or ""
        if "room_admin" not in users_schema:
            db.execute(
                "CREATE TABLE users_migrated ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "username TEXT NOT NULL UNIQUE COLLATE NOCASE,"
                "display_name TEXT NOT NULL,"
                "role TEXT NOT NULL DEFAULT 'player' CHECK(role IN ('player','room_admin')),"
                "password_hash TEXT NOT NULL,"
                "enabled INTEGER NOT NULL DEFAULT 1,"
                "created_at TEXT NOT NULL,"
                "updated_at TEXT NOT NULL)"
            )
            db.execute(
                "INSERT INTO users_migrated(id,username,display_name,role,password_hash,enabled,created_at,updated_at) "
                "SELECT id,username,display_name,CASE WHEN lower(role)='editor' THEN 'room_admin' ELSE 'player' END,"
                "password_hash,enabled,created_at,updated_at FROM users"
            )
            db.execute("DROP TABLE users")
            db.execute("ALTER TABLE users_migrated RENAME TO users")
        db.execute(
            "UPDATE users SET role=CASE WHEN lower(role) IN ('room_admin','editor') "
            "THEN 'room_admin' ELSE 'player' END, "
            "display_name=CASE WHEN trim(display_name)='' THEN username ELSE display_name END"
        )
        username = os.getenv("ADMIN_USERNAME", "admin").strip() or "admin"
        password = os.getenv("ADMIN_PASSWORD", "")
        existing = db.execute("SELECT id FROM admins LIMIT 1").fetchone()
        if existing is None:
            if len(password) < MIN_PASSWORD_LENGTH:
                raise RuntimeError(
                    f"首次管理员密码必须至少为 {MIN_PASSWORD_LENGTH} 个字符"
                )
            db.execute(
                "INSERT INTO admins(username, password_hash, updated_at) VALUES(?, ?, ?)",
                (username, password_hasher.hash(password), iso_time(utc_now())),
            )
        if db.execute("SELECT 1 FROM rooms WHERE room_id=?", (ACTIVE_GAME_ROOM_ID,)).fetchone() is None:
            now = iso_time(utc_now())
            db.execute(
                "INSERT INTO rooms(room_id,name,description,scenario_id,protocol_profile,seat_limits,seat_parameters,mode,ai_difficulty,ai_provider,ai_model,ai_resolved_model,config_version,intel_stale_after_sec,intel_archive_after_sec,status,enabled,created_at,updated_at) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (ACTIVE_GAME_ROOM_ID, "主推演室", "默认联网推演房间", "default", "native",
                 json.dumps(DEFAULT_SEAT_LIMITS, ensure_ascii=False),
                 json.dumps({}, ensure_ascii=False), "pvp", "normal", "rules", "", "", 1,
                 10.0, 120.0, "stopped", 1, now, now),
            )
        ai_config_row = db.execute(
            "SELECT provider, base_url, model, config_version FROM ai_config WHERE config_id=1"
        ).fetchone()
        if ai_config_row is None:
            config = environment_ai_config()
            db.execute(
                "INSERT INTO ai_config(config_id,provider,base_url,model,config_version,updated_at) "
                "VALUES(1,?,?,?,?,?)",
                (config["provider"], config["baseUrl"], config["model"], 1, iso_time(utc_now())),
            )
        elif (
            ai_config_row["provider"] == "auto"
            and ai_config_row["base_url"] == DEFAULT_OLLAMA_BASE_URL
            and ai_config_row["model"] == LEGACY_OLLAMA_MODEL
            and ai_config_row["config_version"] == 1
        ):
            db.execute(
                "UPDATE ai_config SET model=?, config_version=config_version + 1, updated_at=? "
                "WHERE config_id=1 AND provider='auto' AND base_url=? AND model=? "
                "AND config_version=1",
                (DEFAULT_OLLAMA_MODEL, iso_time(utc_now()), DEFAULT_OLLAMA_BASE_URL,
                 LEGACY_OLLAMA_MODEL),
            )
        # Room AI configuration was introduced after the legacy global
        # provider.  Migrate existing PVE rooms exactly once from that global
        # tuple; PVP rooms intentionally remain rules-only.
        if "ai_provider" in added_room_ai_columns:
            global_row = db.execute(
                "SELECT provider, model FROM ai_config WHERE config_id=1"
            ).fetchone()
            global_provider = str(global_row["provider"]) if global_row else "rules"
            global_room_provider = "ollama" if global_provider in {"auto", "ollama"} else "rules"
            global_model = str(global_row["model"]) if global_row else ""
            db.execute(
                "UPDATE rooms SET ai_provider=CASE WHEN mode='pve' THEN ? ELSE 'rules' END, "
                "ai_model=CASE WHEN mode='pve' THEN ? ELSE '' END, ai_resolved_model=''",
                (global_room_provider, global_model),
            )
        db.execute(
            "UPDATE rooms SET ai_provider='rules', ai_model='', ai_resolved_model='' "
            "WHERE mode='pvp'"
        )
        db.execute(
            "UPDATE room_operations SET state='failed', acknowledged_at=? "
            "WHERE room_id!=? AND state='pending'",
            (iso_time(utc_now()), ACTIVE_GAME_ROOM_ID),
        )


@app.middleware("http")
async def security_headers(request: Request, call_next):
    response = await call_next(request)
    response.headers["X-Content-Type-Options"] = "nosniff"
    response.headers["X-Frame-Options"] = "DENY"
    response.headers["Referrer-Policy"] = "no-referrer"
    response.headers["Content-Security-Policy"] = (
        "default-src 'self'; style-src 'self'; script-src 'self'; "
        "img-src 'self' data:; connect-src 'self'"
    )
    return response


def login_client_ip(request: Request) -> str:
    peer_text = request.client.host if request.client else "local"
    try:
        peer = ipaddress.ip_address(peer_text)
    except ValueError:
        return peer_text
    if not any(peer in network for network in LOGIN_TRUSTED_PROXY_NETWORKS):
        return peer.compressed
    forwarded = request.headers.get("x-forwarded-for", "")
    try:
        chain = [ipaddress.ip_address(part.strip()) for part in forwarded.split(",") if part.strip()]
    except ValueError:
        return peer.compressed
    for candidate in reversed(chain):
        if not any(candidate in network for network in LOGIN_TRUSTED_PROXY_NETWORKS):
            return candidate.compressed
    return chain[0].compressed if chain else peer.compressed


def enforce_login_limit(subject_type: str, subject: str, ip_address: str) -> None:
    now = time.time()
    cutoff = now - LOGIN_WINDOW_SECONDS
    normalized_subject = subject.strip().casefold()
    with database() as db:
        db.execute("DELETE FROM login_failures WHERE failed_at <= ?", (cutoff,))
        subject_row = db.execute(
            "SELECT COUNT(*) AS count, MIN(failed_at) AS oldest FROM login_failures "
            "WHERE subject_type=? AND subject=? AND ip_address=? AND failed_at>?",
            (subject_type, normalized_subject, ip_address, cutoff),
        ).fetchone()
        ip_row = db.execute(
            "SELECT COUNT(*) AS count, MIN(failed_at) AS oldest FROM login_failures "
            "WHERE ip_address=? AND failed_at>?",
            (ip_address, cutoff),
        ).fetchone()
    limited_rows = []
    if subject_row["count"] >= LOGIN_SUBJECT_IP_FAILURE_LIMIT:
        limited_rows.append(subject_row)
    if ip_row["count"] >= LOGIN_IP_FAILURE_LIMIT:
        limited_rows.append(ip_row)
    if limited_rows:
        oldest = min(float(row["oldest"]) for row in limited_rows)
        retry_after = max(1, math.ceil(LOGIN_WINDOW_SECONDS - (now - oldest)))
        raise HTTPException(
            status_code=429,
            detail="登录尝试过于频繁，请稍后再试",
            headers={"Retry-After": str(retry_after)},
        )


def record_login_failure(subject_type: str, subject: str, ip_address: str) -> None:
    now = time.time()
    with database() as db:
        db.execute("BEGIN IMMEDIATE")
        db.execute(
            "DELETE FROM login_failures WHERE failed_at <= ?",
            (now - LOGIN_WINDOW_SECONDS,),
        )
        db.execute(
            "INSERT INTO login_failures(subject_type,subject,ip_address,failed_at) "
            "VALUES(?,?,?,?)",
            (subject_type, subject.strip().casefold(), ip_address, now),
        )


def clear_login_failures(subject_type: str, subject: str, ip_address: str) -> None:
    with database() as db:
        db.execute(
            "DELETE FROM login_failures WHERE subject_type=? AND subject=? AND ip_address=?",
            (subject_type, subject.strip().casefold(), ip_address),
        )


def verify_password(password_hash: str, password: str) -> bool:
    try:
        return password_hasher.verify(password_hash, password)
    except VerificationError:
        return False


def create_session(db: sqlite3.Connection, subject_type: str, subject_id: int) -> str:
    token = secrets.token_urlsafe(40)
    now = utc_now()
    db.execute("DELETE FROM sessions WHERE expires_at <= ?", (iso_time(now),))
    db.execute(
        "INSERT INTO sessions(token_hash, subject_type, subject_id, expires_at, created_at) "
        "VALUES(?, ?, ?, ?, ?)",
        (
            token_digest(token),
            subject_type,
            subject_id,
            iso_time(now + timedelta(hours=SESSION_HOURS)),
            iso_time(now),
        ),
    )
    return token


def assert_player_session_available(db: sqlite3.Connection, subject_id: int) -> None:
    """Enforce the one-account/one-client invariant atomically at login time."""
    now = iso_time(utc_now())
    db.execute("DELETE FROM sessions WHERE expires_at <= ?", (now,))
    active = db.execute(
        "SELECT 1 FROM sessions WHERE subject_type='player' AND subject_id=? AND expires_at>? LIMIT 1",
        (subject_id, now),
    ).fetchone()
    if active is not None:
        raise HTTPException(status_code=409, detail="USER_ALREADY_ONLINE")


def bearer_token(authorization: str | None) -> str:
    if not authorization or not authorization.startswith("Bearer "):
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="请先登录")
    return authorization[7:].strip()


def require_admin(authorization: str | None = Header(default=None)) -> sqlite3.Row:
    digest = token_digest(bearer_token(authorization))
    with database() as db:
        row = db.execute(
            "SELECT a.id, a.username, a.password_hash FROM sessions s "
            "JOIN admins a ON a.id = s.subject_id "
            "WHERE s.token_hash = ? AND s.subject_type = 'admin' AND s.expires_at > ?",
            (digest, iso_time(utc_now())),
        ).fetchone()
    if row is None:
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="登录已失效")
    return row


def require_internal_key(x_internal_key: str | None) -> None:
    if not secrets.compare_digest(x_internal_key or "", INTERNAL_KEY):
        raise HTTPException(status_code=403, detail="内部认证失败")


def admin_session_is_valid(admin_id: int, session_digest: str) -> bool:
    with database() as db:
        row = db.execute(
            "SELECT 1 FROM sessions WHERE token_hash=? AND subject_type='admin' "
            "AND subject_id=? AND expires_at>?",
            (session_digest, admin_id, iso_time(utc_now())),
        ).fetchone()
    return row is not None


def record_terminal_audit(admin_id: int, event: str, outcome: str) -> None:
    with database() as db:
        db.execute(
            "INSERT INTO terminal_audit(admin_id,action,event,outcome,created_at) "
            "VALUES(?,?,?,?,?)",
            (admin_id, f"shell.{event}:{outcome}", event, outcome, iso_time(utc_now())),
        )


def issue_terminal_ticket(admin_id: int, session_digest: str) -> str:
    now = time.monotonic()
    ticket = secrets.token_urlsafe(32)
    with terminal_tickets_lock:
        expired = [key for key, value in terminal_tickets.items() if value[2] <= now]
        for key in expired:
            del terminal_tickets[key]
        terminal_tickets[ticket] = (admin_id, session_digest, now + WEB_SHELL_TICKET_SECONDS)
    return ticket


def consume_terminal_ticket(ticket: str | None) -> tuple[int, str] | None:
    if not ticket:
        return None
    now = time.monotonic()
    with terminal_tickets_lock:
        value = terminal_tickets.pop(ticket, None)
    if value is None or value[2] <= now:
        return None
    return value[0], value[1]


def terminal_origin_allowed(origin: str | None) -> bool:
    if origin is None or origin == "null":
        return False
    try:
        parsed = urlsplit(origin)
        port = parsed.port
    except ValueError:
        return False
    if (
        parsed.scheme not in {"http", "https"}
        or parsed.hostname is None
        or parsed.username is not None
        or parsed.password is not None
        or parsed.path
        or parsed.query
        or parsed.fragment
        or "*" in origin
    ):
        return False
    host = f"[{parsed.hostname}]" if ":" in parsed.hostname else parsed.hostname
    normalized = f"{parsed.scheme}://{host}{f':{port}' if port is not None else ''}"
    return origin == normalized and origin in WEB_SHELL_ALLOWED_ORIGINS


def consume_terminal_identity(origin: str | None, ticket: str | None) -> tuple[int, str] | None:
    if not terminal_origin_allowed(origin):
        return None
    return consume_terminal_ticket(ticket)


def terminal_ticket_from_protocol(header: str | None) -> str | None:
    if header is None:
        return None
    protocols = [part.strip() for part in header.split(",")]
    if len(protocols) != 2 or protocols[0] != "wargame-terminal":
        return None
    return protocols[1] or None


def invalidate_terminal_tickets(session_digest: str | None = None) -> None:
    with terminal_tickets_lock:
        if session_digest is None:
            terminal_tickets.clear()
            return
        for ticket, value in list(terminal_tickets.items()):
            if secrets.compare_digest(value[1], session_digest):
                del terminal_tickets[ticket]


def reserve_terminal_session() -> bool:
    global active_terminal_sessions
    with active_terminal_sessions_lock:
        if active_terminal_sessions >= WEB_SHELL_MAX_SESSIONS:
            return False
        active_terminal_sessions += 1
        return True


def release_terminal_session() -> None:
    global active_terminal_sessions
    with active_terminal_sessions_lock:
        active_terminal_sessions = max(0, active_terminal_sessions - 1)


def public_user(row: sqlite3.Row) -> dict:
    result = {
        "id": row["id"],
        "username": row["username"],
        "displayName": normalized_display_name(row["username"], row["display_name"]),
        "role": normalize_user_role(row["role"]),
        "enabled": bool(row["enabled"]),
        "createdAt": row["created_at"],
        "updatedAt": row["updated_at"],
    }
    if "online" in row.keys():
        result["online"] = bool(row["online"])
    return result


def public_ai_config(row: sqlite3.Row) -> dict:
    scheme, host, port = ollama_base_url_parts(row["base_url"])
    return {
        "provider": row["provider"],
        "model": row["model"],
        "ollamaScheme": scheme,
        "ollamaHost": host,
        "ollamaPort": port,
        "baseUrl": row["base_url"],
        "configVersion": int(row["config_version"]),
        "updatedAt": row["updated_at"],
    }


def ollama_model_inventory(base_url: str, refresh: bool = False) -> dict:
    """Read the Ollama model catalog from the account-service host.

    The browser never talks to Ollama directly.  Keep a short-lived process
    cache so opening the monitor page does not create a request storm.
    """
    global _ollama_models_cache
    normalized = base_url.rstrip("/")
    now = utc_now()
    with _ollama_models_lock:
        cached = dict(_ollama_models_cache)
    if (
        not refresh
        and cached.get("baseUrl") == normalized
        and isinstance(cached.get("checkedAt"), str)
        and cached.get("checkedAt")
    ):
        try:
            checked = datetime.fromisoformat(str(cached["checkedAt"]).replace("Z", "+00:00"))
            if now - checked < timedelta(seconds=30):
                return cached
        except ValueError:
            pass

    checked_at = iso_time(now)
    models: list[dict[str, object]] = []
    model_names: list[str] = []
    error = ""
    connected = False
    try:
        request = urllib.request.Request(
            f"{normalized}/api/tags",
            headers={"Accept": "application/json", "User-Agent": "wargame-account"},
            method="GET",
        )
        with urllib.request.urlopen(request, timeout=OLLAMA_PROBE_TIMEOUT_SECONDS) as response:
            content_length = response.headers.get("Content-Length")
            if content_length and int(content_length) > OLLAMA_PROBE_MAX_BYTES:
                raise ValueError("Ollama 返回过大")
            payload = response.read(OLLAMA_PROBE_MAX_BYTES + 1)
            if len(payload) > OLLAMA_PROBE_MAX_BYTES:
                raise ValueError("Ollama 返回过大")
        document = json.loads(payload.decode("utf-8"))
        raw_models = document.get("models") if isinstance(document, dict) else None
        if not isinstance(raw_models, list):
            raise ValueError("Ollama 返回格式无效")
        for item in raw_models[:512]:
            if isinstance(item, str):
                name = item.strip()
                entry: dict[str, object] = {"name": name}
            elif isinstance(item, dict):
                name = item.get("name") if isinstance(item.get("name"), str) else ""
                name = name.strip()
                entry = {"name": name}
                if isinstance(item.get("size"), (int, float)):
                    entry["size"] = int(item["size"])
                if isinstance(item.get("modified_at"), str):
                    entry["modifiedAt"] = item["modified_at"]
            else:
                continue
            if not name or name in model_names or len(name) > 128 or any(c in name for c in "\r\n"):
                continue
            model_names.append(name)
            models.append(entry)
        connected = True
    except (OSError, ValueError, UnicodeError, json.JSONDecodeError) as exc:
        error = str(exc)[:256] or "Ollama 连接失败"
        if isinstance(exc, urllib.error.HTTPError):
            error = f"HTTP {exc.code}"
        elif isinstance(exc, urllib.error.URLError):
            error = str(exc.reason)[:256]

    result = {
        "baseUrl": normalized,
        "connected": connected,
        "connectionStatus": "connected" if connected else "unavailable",
        "models": models,
        "modelNames": model_names,
        "checkedAt": checked_at,
        "error": error,
    }
    with _ollama_models_lock:
        _ollama_models_cache = result
    return result


def hosted_room_can_change_ai(db: sqlite3.Connection) -> bool:
    row = db.execute(
        "SELECT status FROM rooms WHERE room_id=?", (ACTIVE_GAME_ROOM_ID,)
    ).fetchone()
    if row is not None and row["status"] != "stopped":
        return False
    pending = db.execute(
        "SELECT 1 FROM room_operations WHERE room_id=? AND state='pending' LIMIT 1",
        (ACTIVE_GAME_ROOM_ID,),
    ).fetchone()
    return pending is None


def room_ai_provider(value: object, mode: str = "pvp") -> str:
    candidate = str(value or "").strip().lower()
    if candidate in {"rules", "ollama"}:
        return candidate
    return "rules" if mode != "pve" else "ollama"


def room_ai_model(value: object) -> str:
    if not isinstance(value, str):
        return ""
    model = value.strip()
    return model if len(model) <= 128 and not any(c in model for c in "\r\n") else ""


def public_room(row: sqlite3.Row) -> dict:
    def decode(value: str, fallback: object) -> object:
        try:
            parsed = json.loads(value)
            return parsed if isinstance(parsed, (dict, list)) else fallback
        except (TypeError, json.JSONDecodeError):
            return fallback

    decoded_limits = decode(row["seat_limits"], {})
    seat_limits = dict(DEFAULT_SEAT_LIMITS)
    if isinstance(decoded_limits, dict):
        seat_limits.update(decoded_limits)
    result = {
        "roomId": row["room_id"],
        "name": row["name"],
        "description": row["description"],
        "scenarioId": row["scenario_id"],
        "protocolProfile": row["protocol_profile"] if "protocol_profile" in row.keys() else "native",
        "seatLimits": seat_limits,
        "seatParameters": decode(row["seat_parameters"], {}),
        "mode": row["mode"],
        "aiDifficulty": row["ai_difficulty"],
        "aiProvider": room_ai_provider(row["ai_provider"], row["mode"]),
        "aiModel": room_ai_model(row["ai_model"]),
        "aiResolvedModel": room_ai_model(row["ai_resolved_model"]),
        "configVersion": int(row["config_version"]),
        "intelStaleAfterSec": float(row["intel_stale_after_sec"]),
        "intelArchiveAfterSec": float(row["intel_archive_after_sec"]),
        "status": row["status"],
        "winner": row["winner"],
        "statusReason": row["status_reason"],
        "enabled": bool(row["enabled"]),
        "hostedByGameServer": row["room_id"] == ACTIVE_GAME_ROOM_ID,
        "createdAt": row["created_at"],
        "updatedAt": row["updated_at"],
    }
    if "operation_id" in row.keys() and row["operation_id"]:
        operation = {
            "operationId": row["operation_id"],
            "action": row["operation_action"],
            "expectedStatus": row["operation_expected_status"],
            "state": row["operation_state"],
            "requestedRevision": row["operation_requested_revision"],
            "requestedConfigVersion": row["operation_requested_config_version"],
            "requestedOllamaConfigVersion": row["operation_requested_ollama_config_version"],
            "appliedRevision": row["operation_applied_revision"],
            "code": row["operation_result_code"],
            "requestedAt": row["operation_requested_at"],
            "acknowledgedAt": row["operation_acknowledged_at"],
        }
        result["operation"] = operation
        if operation["state"] == "pending":
            result["pendingOperation"] = operation
    result.update(room_readiness(row["room_id"]))
    return result


def room_rows(db: sqlite3.Connection, enabled_only: bool) -> list[sqlite3.Row]:
    query = """
        SELECT r.*, o.operation_id, o.action AS operation_action,
               o.expected_status AS operation_expected_status,
               o.state AS operation_state, o.requested_at AS operation_requested_at,
               o.acknowledged_at AS operation_acknowledged_at,
               o.requested_revision AS operation_requested_revision,
               o.requested_config_version AS operation_requested_config_version,
               o.requested_ollama_config_version AS operation_requested_ollama_config_version,
               o.applied_revision AS operation_applied_revision,
               o.result_code AS operation_result_code
        FROM rooms r
        LEFT JOIN room_operations o ON o.operation_id = (
            SELECT latest.operation_id FROM room_operations latest
            WHERE latest.room_id = r.room_id
            ORDER BY latest.requested_at DESC, latest.operation_id DESC LIMIT 1
        )
    """
    if enabled_only:
        query += " WHERE r.enabled=1"
    query += " ORDER BY r.name COLLATE NOCASE"
    return db.execute(query).fetchall()


def public_presence(row: sqlite3.Row) -> dict:
    return {
        "userId": row["user_id"],
        "username": row["username"],
        "displayName": normalized_display_name(row["username"], row["display_name"]),
        "seatId": row["seat_id"],
        "seatType": row["seat_type"],
        "side": row["side"],
        "connectedAt": row["connected_at"],
        "lastSeenAt": row["last_seen_at"],
        "updatedAt": row["updated_at"],
    }


def sync_presence(db: sqlite3.Connection, room_id: str, occupants: list[dict[str, object]]) -> None:
    """Replace the authoritative game-server presence snapshot for one room."""
    now = iso_time(utc_now())
    seen: set[int] = set()
    for occupant in occupants:
        try:
            user_id = int(occupant.get("userId", 0))
        except (TypeError, ValueError):
            continue
        if user_id <= 0 or user_id in seen:
            continue
        username = str(occupant.get("username", ""))[:64]
        display_name = normalized_display_name(username, occupant.get("displayName", ""))[:64]
        if not username:
            continue
        seen.add(user_id)
        db.execute(
            "INSERT INTO room_presence(room_id,user_id,username,display_name,seat_id,seat_type,side,connected_at,last_seen_at,updated_at) "
            "VALUES(?,?,?,?,?,?,?,?,?,?) "
            "ON CONFLICT(room_id,user_id) DO UPDATE SET username=excluded.username,display_name=excluded.display_name,"
            "seat_id=excluded.seat_id,seat_type=excluded.seat_type,side=excluded.side,connected_at=excluded.connected_at,"
            "last_seen_at=excluded.last_seen_at,updated_at=excluded.updated_at",
            (room_id, user_id, username, display_name,
             str(occupant.get("seatId", ""))[:96], str(occupant.get("seatType", ""))[:32],
             str(occupant.get("side", ""))[:16], str(occupant.get("connectedAt", now))[:64],
             str(occupant.get("lastSeenAt", now))[:64], now),
        )
    if seen:
        placeholders = ",".join("?" for _ in seen)
        db.execute(
            f"DELETE FROM room_presence WHERE room_id=? AND user_id NOT IN ({placeholders})",
            (room_id, *seen),
        )
    else:
        db.execute("DELETE FROM room_presence WHERE room_id=?", (room_id,))


def read_game_status() -> dict:
    try:
        value = json.loads(GAME_STATUS_PATH.read_text(encoding="utf-8"))
        if not isinstance(value, dict):
            return {}
        updated_at = value.get("updatedAt")
        if isinstance(updated_at, str):
            updated = datetime.fromisoformat(updated_at.replace("Z", "+00:00"))
            if utc_now() - updated > timedelta(seconds=6):
                value["status"] = "stale"
        return value
    except (OSError, ValueError, json.JSONDecodeError):
        return {}


def redacted_ai_status(game_status: dict, configured_ai: dict | None = None) -> dict:
    raw_ai = game_status.get("ai")
    ai = raw_ai if isinstance(raw_ai, dict) else {}
    raw_room = game_status.get("roomState")
    room = raw_room if isinstance(raw_room, dict) else {}

    def allowed_text(value: object, values: set[str]) -> str:
        return value if isinstance(value, str) and value in values else "unknown"

    def nonnegative_count(value: object) -> int:
        return value if isinstance(value, int) and not isinstance(value, bool) and value >= 0 else 0

    provider = allowed_text(ai.get("provider"), {"rules", "auto", "ollama"})
    if provider == "unknown" and isinstance(configured_ai, dict):
        provider = allowed_text(configured_ai.get("provider"), {"rules", "auto", "ollama"})
    connection_status = allowed_text(
        ai.get("connectionStatus"),
        {"disabled", "checking", "connected", "model_missing", "unavailable", "timeout",
         "http_error", "invalid_json", "schema_invalid", "response_too_large", "configuration_error"},
    )
    if connection_status == "unknown" and provider == "rules":
        connection_status = "disabled"
    model = ai.get("model") if isinstance(ai.get("model"), str) else ""
    base_url = ai.get("baseUrl") if isinstance(ai.get("baseUrl"), str) else ""
    if isinstance(configured_ai, dict):
        model = model or str(configured_ai.get("model", ""))
        base_url = base_url or str(configured_ai.get("baseUrl", ""))

    return {
        "active": room.get("roomMode") == "pve",
        "provider": provider,
        "selectedProvider": allowed_text(
            ai.get("selectedProvider"), {"rules", "ollama"}
        ),
        "selectedModel": ai.get("selectedModel")
        if isinstance(ai.get("selectedModel"), str)
        else "",
        "model": model,
        "baseUrl": base_url,
        "effectiveEngine": allowed_text(ai.get("effectiveEngine"), {"rules", "ollama"}),
        "connectionStatus": connection_status,
        "configVersion": nonnegative_count(ai.get("configVersion")),
        "configApplied": bool(ai.get("configApplied")),
        "lastProbeAt": ai.get("lastProbeAt") if isinstance(ai.get("lastProbeAt"), str) else "",
        "probeFailureClass": allowed_text(
            ai.get("probeFailureClass"),
            {"busy", "unavailable", "timeout", "http_error", "model_missing", "invalid_json",
             "schema_invalid", "response_too_large", "configuration_error"},
        ),
        "requests": nonnegative_count(ai.get("requests")),
        "successes": nonnegative_count(ai.get("successes")),
        "failures": nonnegative_count(ai.get("failures")),
        "lastFailureClass": allowed_text(
            ai.get("lastFailureClass"),
            {"rules_disabled", "busy", "unavailable", "timeout", "http_error", "model_missing",
             "response_too_large", "invalid_json", "schema_invalid", "stale_response",
             "configuration_error"},
        ),
        "stickyRules": bool(ai.get("stickyRules")),
        "fallbackReason": ai.get("fallbackReason")
        if isinstance(ai.get("fallbackReason"), str)
        else "",
        "roomConfigVersion": nonnegative_count(
            ai.get("roomConfigVersion") or room.get("configVersion")
        ),
        "resolvedModel": ai.get("resolvedModel")
        if isinstance(ai.get("resolvedModel"), str)
        else "",
    }


def room_ready_for_start(room_id: str) -> bool:
    """Wait for the next monitor snapshot when seat readiness just changed."""
    deadline = time.monotonic() + MONITOR_READY_WAIT_SECONDS
    while True:
        game_room = read_game_status().get("roomState")
        if isinstance(game_room, dict) and game_room.get("roomId") == room_id:
            if bool(game_room.get("readyForStart")):
                return True
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.05)


def room_readiness(room_id: str) -> dict[str, bool]:
    game_room = read_game_status().get("roomState")
    if not isinstance(game_room, dict) or game_room.get("roomId") != room_id:
        return {"redCommanderReady": False, "blueCommanderReady": False, "readyForStart": False}
    readiness = {"redCommanderReady": False, "blueCommanderReady": False,
                 "readyForStart": bool(game_room.get("readyForStart"))}
    seats = game_room.get("seats")
    if not isinstance(seats, list):
        return readiness
    for seat in seats:
        if not isinstance(seat, dict) or seat.get("seatType") != "commander":
            continue
        if seat.get("side") == "red":
            readiness["redCommanderReady"] = bool(seat.get("ready"))
        elif seat.get("side") == "blue":
            readiness["blueCommanderReady"] = bool(seat.get("ready"))
    return readiness


def pending_operation_timed_out(operation: sqlite3.Row, now: datetime) -> bool:
    try:
        requested_at = datetime.fromisoformat(operation["requested_at"].replace("Z", "+00:00"))
    except (TypeError, ValueError):
        return True
    if requested_at.tzinfo is None:
        requested_at = requested_at.replace(tzinfo=timezone.utc)
    return now - requested_at >= timedelta(seconds=ROOM_OPERATION_TIMEOUT_SECONDS)


def room_lifecycle_revision(room_id: str) -> int:
    game_room = read_game_status().get("roomState")
    if not isinstance(game_room, dict) or game_room.get("roomId") != room_id:
        return 0
    revision = game_room.get("lifecycleRevision")
    return revision if isinstance(revision, int) and not isinstance(revision, bool) and revision > 0 else 0


def read_monitor_events(category: str, limit: int) -> list[dict]:
    if not GAME_EVENTS_PATH.exists():
        return []
    try:
        with GAME_EVENTS_PATH.open("rb") as handle:
            handle.seek(max(0, GAME_EVENTS_PATH.stat().st_size - 256 * 1024))
            if handle.tell() > 0:
                handle.readline()
            lines = handle.read().decode("utf-8", errors="replace").splitlines()
    except OSError:
        return []

    output: list[dict] = []
    for line in reversed(lines):
        try:
            entry = json.loads(line)
        except json.JSONDecodeError:
            continue
        if not isinstance(entry, dict):
            continue
        if category != "all" and entry.get("category") != category:
            continue
        output.append(entry)
        if len(output) >= limit:
            break
    output.reverse()
    return output


def ai_conversation_history_root() -> Path:
    return DATA_DIR / "ai-planning-history"


@app.get("/")
def index() -> FileResponse:
    return FileResponse(STATIC_DIR / "index.html")


@app.get("/app.css")
def app_css() -> FileResponse:
    return FileResponse(STATIC_DIR / "app.css", media_type="text/css")


@app.get("/app.js")
def app_js() -> FileResponse:
    return FileResponse(STATIC_DIR / "app.js", media_type="application/javascript")


@app.get("/api/health")
def health() -> dict:
    with database() as db:
        db.execute("SELECT 1").fetchone()
    return {
        "status": "ok",
        "service": "account-web",
        "version": WARGAME_VERSION,
        "sourceDigest": WARGAME_SOURCE_DIGEST,
    }


@app.post("/api/admin/login")
def admin_login(body: LoginBody, request: Request) -> dict:
    ip_address = login_client_ip(request)
    subject = body.username.strip().casefold()
    enforce_login_limit("admin", subject, ip_address)
    with database() as db:
        row = db.execute(
            "SELECT id, username, password_hash FROM admins WHERE username = ?",
            (body.username.strip(),),
        ).fetchone()
        if row is None or not verify_password(row["password_hash"], body.password):
            record_login_failure("admin", subject, ip_address)
            raise HTTPException(status_code=401, detail="管理员用户名或密码错误")
        token = create_session(db, "admin", row["id"])
    clear_login_failures("admin", subject, ip_address)
    return {"token": token, "username": row["username"], "expiresInHours": SESSION_HOURS}


@app.post("/api/client/login")
def client_login(body: LoginBody, request: Request) -> dict:
    ip_address = login_client_ip(request)
    subject = body.username.strip().casefold()
    enforce_login_limit("player", subject, ip_address)
    with database() as db:
        row = db.execute(
            "SELECT * FROM users WHERE username = ? COLLATE NOCASE",
            (body.username.strip(),),
        ).fetchone()
        if row is None or not bool(row["enabled"]) or not verify_password(row["password_hash"], body.password):
            record_login_failure("player", subject, ip_address)
            raise HTTPException(status_code=401, detail="用户名或密码错误，或账号已停用")
        assert_player_session_available(db, row["id"])
        try:
            token = create_session(db, "player", row["id"])
        except sqlite3.IntegrityError as exc:
            # A concurrent login may win the unique-index race after the
            # explicit availability check.
            raise HTTPException(status_code=409, detail="USER_ALREADY_ONLINE") from exc
    clear_login_failures("player", subject, ip_address)
    return {
        "token": token,
        "username": row["username"],
        "displayName": normalized_display_name(row["username"], row["display_name"]),
        "role": normalize_user_role(row["role"]),
        "gameWebSocketUrl": PUBLIC_WS_URL,
        "gameDataPlane": "websocket-authoritative",
        "sessionPolicy": "single-client",
        "expiresInHours": SESSION_HOURS,
    }


@app.get("/api/admin/me")
def admin_me(admin: sqlite3.Row = Depends(require_admin)) -> dict:
    return {"username": admin["username"]}


@app.get("/api/admin/ai-config")
def admin_ai_config(_: sqlite3.Row = Depends(require_admin)) -> dict:
    with database() as db:
        row = db.execute("SELECT * FROM ai_config WHERE config_id=1").fetchone()
    if row is None:
        raise HTTPException(status_code=503, detail="AI 配置尚未初始化")
    return {"aiConfig": public_ai_config(row)}


@app.get("/api/admin/ollama-config")
def admin_ollama_config(_: sqlite3.Row = Depends(require_admin)) -> dict:
    with database() as db:
        row = db.execute("SELECT * FROM ai_config WHERE config_id=1").fetchone()
    if row is None:
        raise HTTPException(status_code=503, detail="Ollama 端点尚未初始化")
    config = public_ai_config(row)
    return {"ollamaConfig": config, "aiConfig": config}


@app.get("/api/admin/ollama-models")
def admin_ollama_models(
    refresh: bool = Query(default=False),
    _: sqlite3.Row = Depends(require_admin),
) -> dict:
    with database() as db:
        row = db.execute("SELECT base_url FROM ai_config WHERE config_id=1").fetchone()
    if row is None:
        raise HTTPException(status_code=503, detail="Ollama 端点尚未初始化")
    return ollama_model_inventory(row["base_url"], refresh=refresh)


def _update_global_endpoint(
    body: OllamaEndpointBody,
    admin: sqlite3.Row,
    provider: str | None = None,
    model: str | None = None,
) -> dict:
    try:
        base_url = build_ollama_base_url(body.ollama_scheme, body.ollama_host, body.ollama_port)
    except ValueError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    with database() as db:
        if not hosted_room_can_change_ai(db):
            raise HTTPException(status_code=409, detail="托管房间只有停止状态才能修改 Ollama 配置")
        existing = db.execute("SELECT * FROM ai_config WHERE config_id=1").fetchone()
        if existing is None:
            raise HTTPException(status_code=503, detail="Ollama 端点尚未初始化")
        next_provider = provider if provider is not None else body.provider
        if next_provider is None:
            next_provider = existing["provider"]
        next_model = model if model is not None else body.model
        if next_model is None:
            next_model = existing["model"]
        changed = base_url != existing["base_url"] or next_provider != existing["provider"] or next_model != existing["model"]
        version = int(existing["config_version"]) + (1 if changed else 0)
        now = iso_time(utc_now())
        db.execute(
            "UPDATE ai_config SET provider=?,base_url=?,model=?,config_version=?,updated_at=? WHERE config_id=1",
            (next_provider, base_url, next_model, version, now),
        )
        if changed:
            db.execute(
                "UPDATE rooms SET ai_resolved_model='', updated_at=? WHERE room_id=? AND status='stopped'",
                (now, ACTIVE_GAME_ROOM_ID),
            )
        row = db.execute("SELECT * FROM ai_config WHERE config_id=1").fetchone()
    with _ollama_models_lock:
        if changed:
            _ollama_models_cache["checkedAt"] = ""
    config = public_ai_config(row)
    return {"ollamaConfig": config, "aiConfig": config, "message": "Ollama 配置已保存，停止房间开启时生效"}


@app.put("/api/admin/ollama-config")
def update_ollama_config(
    body: OllamaEndpointBody,
    admin: sqlite3.Row = Depends(require_admin),
) -> dict:
    return _update_global_endpoint(body, admin)


@app.put("/api/admin/ai-config")
def update_admin_ai_config(
    body: AiConfigBody,
    admin: sqlite3.Row = Depends(require_admin),
) -> dict:
    return _update_global_endpoint(
        OllamaEndpointBody(
            ollama_scheme=body.ollama_scheme,
            ollama_host=body.ollama_host,
            ollama_port=body.ollama_port,
            provider=body.provider,
            model=body.model,
        ),
        admin,
        provider=body.provider,
        model=body.model,
    )


@app.get("/api/admin/users")
def list_users(_: sqlite3.Row = Depends(require_admin)) -> dict:
    with database() as db:
        rows = db.execute(
            "SELECT u.*, EXISTS(SELECT 1 FROM sessions s WHERE s.subject_type='player' "
            "AND s.subject_id=u.id AND s.expires_at>?) AS online "
            "FROM users u ORDER BY u.username COLLATE NOCASE",
            (iso_time(utc_now()),),
        ).fetchall()
    return {"users": [public_user(row) for row in rows]}


@app.post("/api/admin/users/{user_id}/kick", status_code=202)
def kick_user_offline(
    user_id: int,
    body: KickBody,
    admin: sqlite3.Row = Depends(require_admin),
) -> dict:
    now = iso_time(utc_now())
    reason = body.reason.strip() or "管理员强制下线"
    with database() as db:
        user = db.execute("SELECT id, username FROM users WHERE id=?", (user_id,)).fetchone()
        if user is None:
            raise HTTPException(status_code=404, detail="账号不存在")
        active = db.execute(
            "SELECT 1 FROM sessions WHERE subject_type='player' AND subject_id=? AND expires_at>? LIMIT 1",
            (user_id, now),
        ).fetchone()
        if active is None:
            raise HTTPException(status_code=409, detail="该账号当前没有在线会话")
        pending = db.execute(
            "SELECT id FROM user_kick_requests WHERE user_id=? AND processed_at IS NULL LIMIT 1",
            (user_id,),
        ).fetchone()
        if pending is not None:
            return {"accepted": True, "requestId": pending["id"], "message": "下线请求已在处理中"}
        db.execute("DELETE FROM sessions WHERE subject_type='player' AND subject_id=?", (user_id,))
        cursor = db.execute(
            "INSERT INTO user_kick_requests(user_id,reason,requested_by,requested_at) VALUES(?,?,?,?)",
            (user_id, reason, admin["id"], now),
        )
        db.execute(
            "INSERT INTO terminal_audit(admin_id,action,created_at) VALUES(?,?,?)",
            (admin["id"], f"user_kick:{user_id}:{reason}", now),
        )
    return {"accepted": True, "requestId": cursor.lastrowid, "message": "已发送强制下线请求"}


@app.get("/api/admin/monitor/overview")
def monitor_overview(_: sqlite3.Row = Depends(require_admin)) -> dict:
    game_status = read_game_status()
    safe_game_status = dict(game_status)
    safe_game_status.pop("ai", None)
    with database() as db:
        ai_config_row = db.execute("SELECT * FROM ai_config WHERE config_id=1").fetchone()
        active_players = db.execute(
            "SELECT COUNT(*) FROM sessions WHERE subject_type='player' AND expires_at > ?",
            (iso_time(utc_now()),),
        ).fetchone()[0]
    configured_ai = public_ai_config(ai_config_row) if ai_config_row is not None else None
    return {
        "accountStatus": "healthy",
        "gameStatus": safe_game_status or {"status": "unknown"},
        "aiConfig": configured_ai or {},
        "aiStatus": redacted_ai_status(game_status, configured_ai),
        "activePlayerSessions": active_players,
        "recentConnections": len(read_monitor_events("connection", 200)),
        "recentMessages": len(read_monitor_events("message", 200)),
    }


@app.get("/api/admin/monitor/events")
def monitor_events(
    category: str = "all",
    limit: int = 100,
    _: sqlite3.Row = Depends(require_admin),
) -> dict:
    if category not in {"all", "connection", "message", "server"}:
        raise HTTPException(status_code=422, detail="不支持的监控类别")
    return {"events": read_monitor_events(category, max(1, min(limit, 200)))}


@app.get("/api/admin/monitor/ai-conversations", response_model=ConversationPage)
def monitor_ai_conversations(
    status: str = Query(default=""),
    limit: int = Query(default=50, ge=1),
    before: str | None = Query(default=None, max_length=512),
    _: sqlite3.Row = Depends(require_admin),
) -> ConversationPage:
    try:
        return list_conversations(
            ai_conversation_history_root(),
            ConversationQuery(status=status, limit=limit, before=before),
        )
    except (InvalidConversationStatus, InvalidMonitorCursor) as exc:
        raise HTTPException(status_code=422, detail="监控历史查询参数无效") from exc


@app.get(
    "/api/admin/monitor/ai-conversations/{conversation_id}",
    response_model=ConversationDetailResponse,
)
def monitor_ai_conversation_detail(
    conversation_id: str,
    _: sqlite3.Row = Depends(require_admin),
) -> ConversationDetailResponse:
    try:
        result = get_conversation(ai_conversation_history_root(), conversation_id)
    except InvalidConversationId as exc:
        raise HTTPException(status_code=422, detail="会话标识无效") from exc
    if result is None:
        raise HTTPException(status_code=404, detail="会话不存在")
    return result


@app.post("/api/admin/monitor/terminal/login")
def terminal_login(
    body: TerminalLoginBody,
    request: Request,
    authorization: str | None = Header(default=None),
    admin: sqlite3.Row = Depends(require_admin),
) -> dict:
    if not WEB_SHELL_ENABLED:
        raise HTTPException(status_code=503, detail="网页 Shell 未在此服务实例启用")
    ip_address = login_client_ip(request)
    subject = admin["username"].strip().casefold()
    enforce_login_limit("admin-terminal", subject, ip_address)
    if not verify_password(admin["password_hash"], body.password):
        record_login_failure("admin-terminal", subject, ip_address)
        record_terminal_audit(admin["id"], "authorize", "failure")
        raise HTTPException(status_code=403, detail="管理员密码错误")
    clear_login_failures("admin-terminal", subject, ip_address)
    session_digest = token_digest(bearer_token(authorization))
    ticket = issue_terminal_ticket(admin["id"], session_digest)
    record_terminal_audit(admin["id"], "authorize", "success")
    return {
        "authenticated": True,
        "terminalTicket": ticket,
        "ticketExpiresInSeconds": WEB_SHELL_TICKET_SECONDS,
        "message": "终端已授权，请在两分钟内建立会话",
    }


def configure_pty() -> None:
    os.setsid()
    fcntl.ioctl(0, termios.TIOCSCTTY, 0)


@app.websocket("/api/admin/monitor/terminal/ws")
async def terminal_websocket(websocket: WebSocket) -> None:
    if not WEB_SHELL_ENABLED:
        await websocket.accept()
        await websocket.close(code=status.WS_1008_POLICY_VIOLATION)
        return

    if not terminal_origin_allowed(websocket.headers.get("origin")):
        await websocket.accept()
        await websocket.close(code=status.WS_1008_POLICY_VIOLATION)
        return
    identity = consume_terminal_ticket(
        terminal_ticket_from_protocol(websocket.headers.get("sec-websocket-protocol"))
    )
    if identity is None:
        await websocket.accept()
        await websocket.close(code=status.WS_1008_POLICY_VIOLATION)
        return
    admin_id, session_digest = identity
    if not admin_session_is_valid(admin_id, session_digest):
        await websocket.accept()
        await websocket.close(code=status.WS_1008_POLICY_VIOLATION)
        return
    if not reserve_terminal_session():
        await websocket.accept()
        await websocket.close(code=status.WS_1013_TRY_AGAIN_LATER)
        return

    master_fd: int | None = None
    slave_fd: int | None = None
    process: subprocess.Popen[bytes] | None = None
    try:
        await websocket.accept(subprotocol="wargame-terminal")
        record_terminal_audit(admin_id, "open", "success")
        master_fd, slave_fd = pty.openpty()
        flags = fcntl.fcntl(master_fd, fcntl.F_GETFL)
        fcntl.fcntl(master_fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)
        process = subprocess.Popen(
            ["/bin/sh", "-i"],
            stdin=slave_fd,
            stdout=slave_fd,
            stderr=slave_fd,
            cwd=SHELL_WORKING_DIR,
            env={
                "HOME": "/app",
                "PATH": "/usr/local/bin:/usr/local/sbin:/usr/bin:/usr/sbin:/bin:/sbin",
                "PS1": "wargame$ ",
                "TERM": "dumb",
            },
            close_fds=True,
            preexec_fn=configure_pty,
        )
        os.close(slave_fd)
        slave_fd = None
        deadline = time.monotonic() + WEB_SHELL_SESSION_SECONDS
        await websocket.send_text(
            "已连接 account-web 容器（wargame 用户）。\r\n"
            "此终端不能访问宿主机、Docker 套接字或 game-server 容器。\r\n"
        )

        while time.monotonic() < deadline:
            message = None
            try:
                with anyio.move_on_after(0.05):
                    message = await websocket.receive_text()
            except WebSocketDisconnect:
                break

            if message is not None:
                if len(message.encode("utf-8")) > 4096:
                    await websocket.send_text("\r\n单次输入不能超过 4096 字节。\r\n")
                    continue
                if not admin_session_is_valid(admin_id, session_digest):
                    await websocket.send_text("\r\n管理员会话已失效，终端已关闭。\r\n")
                    break
                record_terminal_audit(admin_id, "command", "accepted")
                os.write(master_fd, message.encode("utf-8"))

            pty_closed = False
            for _ in range(8):
                try:
                    output = os.read(master_fd, 8192)
                except BlockingIOError:
                    break
                except OSError as exc:
                    # Linux returns EIO when the slave side of a PTY has closed.
                    if exc.errno == errno.EIO:
                        pty_closed = True
                        break
                    raise
                if not output:
                    pty_closed = True
                    break
                await websocket.send_text(output.decode("utf-8", errors="replace"))
            if pty_closed or process.poll() is not None:
                await websocket.send_text("\r\nShell 已退出。\r\n")
                break
        else:
            await websocket.send_text("\r\n终端会话已达到时限，请重新验证管理员密码。\r\n")
    except WebSocketDisconnect:
        record_terminal_audit(admin_id, "disconnect", "success")
    except Exception:  # noqa: BROAD_EXCEPT_OK, BLE001 - boundary audits then re-raises.
        record_terminal_audit(admin_id, "error", "failure")
        raise
    finally:
        if websocket.application_state == WebSocketState.CONNECTED:
            try:
                await websocket.close()
            except RuntimeError:
                pass
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)
        if slave_fd is not None:
            os.close(slave_fd)
        if master_fd is not None:
            os.close(master_fd)
        record_terminal_audit(admin_id, "close", "success")
        release_terminal_session()


@app.post("/api/admin/users", status_code=201)
def create_user(body: UserBody, _: sqlite3.Row = Depends(require_admin)) -> dict:
    if body.password is None:
        raise HTTPException(status_code=422, detail="新账号必须提供密码")
    now = iso_time(utc_now())
    try:
        with database() as db:
            cursor = db.execute(
                "INSERT INTO users(username, display_name, role, password_hash, enabled, created_at, updated_at) "
                "VALUES(?, ?, ?, ?, ?, ?, ?)",
                (
                    body.username,
                    body.display_name,
                    normalize_user_role(body.role),
                    password_hasher.hash(body.password),
                    int(body.enabled),
                    now,
                    now,
                ),
            )
            row = db.execute("SELECT * FROM users WHERE id = ?", (cursor.lastrowid,)).fetchone()
    except sqlite3.IntegrityError as exc:
        raise HTTPException(status_code=409, detail="用户名已存在") from exc
    return {"user": public_user(row)}


@app.put("/api/admin/users/{user_id}")
def update_user(user_id: int, body: UserBody, admin: sqlite3.Row = Depends(require_admin)) -> dict:
    try:
        with database() as db:
            existing = db.execute("SELECT * FROM users WHERE id = ?", (user_id,)).fetchone()
            if existing is None:
                raise HTTPException(status_code=404, detail="账号不存在")
            password_hash = existing["password_hash"]
            if body.password is not None:
                password_hash = password_hasher.hash(body.password)
            stored_role = (
                normalize_user_role(body.role)
                if "role" in body.model_fields_set
                else normalize_user_role(existing["role"])
            )
            db.execute(
                "UPDATE users SET username=?, display_name=?, role=?, password_hash=?, enabled=?, updated_at=? "
                "WHERE id=?",
                (
                    body.username,
                    body.display_name,
                    stored_role,
                    password_hash,
                    int(body.enabled),
                    iso_time(utc_now()),
                    user_id,
                ),
            )
            invalidates_session = (
                body.password is not None
                or not body.enabled
                or body.username.casefold() != existing["username"].casefold()
                or body.display_name != existing["display_name"]
                or stored_role != normalize_user_role(existing["role"])
            )
            if invalidates_session:
                active = db.execute(
                    "SELECT 1 FROM sessions WHERE subject_type='player' AND subject_id=? LIMIT 1",
                    (user_id,),
                ).fetchone()
                db.execute("DELETE FROM sessions WHERE subject_type='player' AND subject_id=?", (user_id,))
                if active is not None:
                    queue_user_invalidation(db, user_id, "账号资料已更新", admin["id"])
            row = db.execute("SELECT * FROM users WHERE id = ?", (user_id,)).fetchone()
    except sqlite3.IntegrityError as exc:
        raise HTTPException(status_code=409, detail="用户名已存在") from exc
    return {"user": public_user(row)}


@app.delete("/api/admin/users/{user_id}")
def delete_user(user_id: int, admin: sqlite3.Row = Depends(require_admin)) -> dict:
    with database() as db:
        existing = db.execute("SELECT id FROM users WHERE id = ?", (user_id,)).fetchone()
        if existing is None:
            raise HTTPException(status_code=404, detail="账号不存在")
        active = db.execute(
            "SELECT 1 FROM sessions WHERE subject_type='player' AND subject_id=? LIMIT 1",
            (user_id,),
        ).fetchone()
        db.execute("DELETE FROM sessions WHERE subject_type='player' AND subject_id=?", (user_id,))
        if active is not None:
            queue_user_invalidation(db, user_id, "账号已删除", admin["id"])
        db.execute("DELETE FROM users WHERE id=?", (user_id,))
    return {"deleted": True}


@app.get("/api/rooms")
def list_public_rooms(x_internal_key: str | None = Header(default=None, alias="X-Internal-Key")) -> dict:
    require_internal_key(x_internal_key)
    with database() as db:
        rows = room_rows(db, enabled_only=True)
    return {"rooms": [public_room(row) for row in rows]}


@app.get("/api/admin/rooms")
def list_rooms(_: sqlite3.Row = Depends(require_admin)) -> dict:
    with database() as db:
        rows = room_rows(db, enabled_only=False)
    game_room = read_game_status().get("roomState")
    rooms = []
    for row in rows:
        room = public_room(row)
        if isinstance(game_room, dict) and game_room.get("roomId") == room["roomId"]:
            room["redReady"] = bool(game_room.get("redReady"))
            room["blueReady"] = bool(game_room.get("blueReady"))
            room["readyForSim"] = bool(game_room.get("readyForSim"))
            room["readyForStart"] = bool(game_room.get("readyForStart"))
        else:
            room["redReady"] = False
            room["blueReady"] = False
            room["readyForSim"] = False
            room["readyForStart"] = False
        rooms.append(room)
    return {"rooms": rooms}


@app.get("/api/admin/rooms/{room_id}/occupants")
def list_room_occupants(room_id: str, _: sqlite3.Row = Depends(require_admin)) -> dict:
    fresh_after = iso_time(utc_now() - timedelta(seconds=8))
    with database() as db:
        room = db.execute("SELECT room_id FROM rooms WHERE room_id=?", (room_id,)).fetchone()
        if room is None:
            raise HTTPException(status_code=404, detail="房间不存在")
        rows = db.execute(
            "SELECT * FROM room_presence WHERE room_id=? AND updated_at>? "
            "ORDER BY seat_id='', side, seat_id, username COLLATE NOCASE",
            (room_id, fresh_after),
        ).fetchall()
    return {"roomId": room_id, "occupants": [public_presence(row) for row in rows]}


@app.post("/api/admin/rooms/{room_id}/occupants/{user_id}/kick", status_code=202)
def kick_room_occupant(
    room_id: str,
    user_id: int,
    body: KickBody,
    admin: sqlite3.Row = Depends(require_admin),
) -> dict:
    now = iso_time(utc_now())
    with database() as db:
        room = db.execute("SELECT room_id FROM rooms WHERE room_id=?", (room_id,)).fetchone()
        if room is None:
            raise HTTPException(status_code=404, detail="房间不存在")
        user = db.execute("SELECT id, username, display_name FROM users WHERE id=?", (user_id,)).fetchone()
        if user is None:
            raise HTTPException(status_code=404, detail="账号不存在")
        presence = db.execute(
            "SELECT 1 FROM room_presence WHERE room_id=? AND user_id=? AND updated_at>?",
            (room_id, user_id, iso_time(utc_now() - timedelta(seconds=8))),
        ).fetchone()
        if presence is None:
            raise HTTPException(status_code=409, detail="该用户当前不在房间内")
        pending = db.execute(
            "SELECT id FROM room_kick_requests WHERE room_id=? AND user_id=? AND processed_at IS NULL LIMIT 1",
            (room_id, user_id),
        ).fetchone()
        if pending is not None:
            return {"accepted": True, "requestId": pending["id"], "message": "移出请求已在处理中"}
        cursor = db.execute(
            "INSERT INTO room_kick_requests(room_id,user_id,reason,requested_by,requested_at) VALUES(?,?,?,?,?)",
            (room_id, user_id, body.reason.strip() or "管理员移出房间", admin["id"], now),
        )
        db.execute(
            "INSERT INTO room_audit(room_id,user_id,admin_id,action,detail,created_at) VALUES(?,?,?,?,?,?)",
            (room_id, user_id, admin["id"], "kick_requested", body.reason.strip() or "管理员移出房间", now),
        )
    return {"accepted": True, "requestId": cursor.lastrowid, "message": "已发送移出请求"}


@app.post("/api/admin/rooms", status_code=201)
def create_room(body: RoomBody, _: sqlite3.Row = Depends(require_admin)) -> dict:
    now = iso_time(utc_now())
    compatibility_limits = dict(DEFAULT_SEAT_LIMITS)
    compatibility_limits.update(body.seat_limits)
    if body.intel_archive_after_sec <= body.intel_stale_after_sec:
        raise HTTPException(status_code=422, detail="情报归档阈值必须大于失联阈值")
    try:
        with database() as db:
            global_ai = db.execute(
                "SELECT provider, model FROM ai_config WHERE config_id=1"
            ).fetchone()
            default_provider = (
                "ollama"
                if global_ai is not None and global_ai["provider"] in {"auto", "ollama"}
                else "rules"
            )
            ai_provider = body.ai_provider or (default_provider if body.mode == "pve" else "rules")
            ai_model = body.ai_model
            if ai_model is None:
                ai_model = str(global_ai["model"]) if body.mode == "pve" and global_ai else ""
            if ai_provider == "rules":
                # Keep a manually entered model for inspection, but it must
                # never make a rules-only room depend on Ollama.
                ai_model = ai_model or ""
            db.execute(
                "INSERT INTO rooms(room_id,name,description,scenario_id,protocol_profile,seat_limits,seat_parameters,mode,ai_difficulty,ai_provider,ai_model,ai_resolved_model,config_version,intel_stale_after_sec,intel_archive_after_sec,status,enabled,created_at,updated_at) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (body.room_id, body.name, body.description, body.scenario_id, body.protocol_profile,
                 json.dumps(compatibility_limits, ensure_ascii=False),
                 json.dumps(body.seat_parameters, ensure_ascii=False), body.mode,
                 body.ai_difficulty, ai_provider, ai_model, "", 1,
                 body.intel_stale_after_sec, body.intel_archive_after_sec, "stopped",
                 int(body.enabled), now, now),
            )
            row = db.execute("SELECT * FROM rooms WHERE room_id=?", (body.room_id,)).fetchone()
    except sqlite3.IntegrityError as exc:
        raise HTTPException(status_code=409, detail="房间 ID 已存在") from exc
    return {"room": public_room(row)}


@app.put("/api/admin/rooms/{room_id}")
def update_room(room_id: str, body: RoomBody, _: sqlite3.Row = Depends(require_admin)) -> dict:
    if room_id != body.room_id:
        raise HTTPException(status_code=422, detail="房间 ID 不允许修改")
    with database() as db:
        existing = db.execute("SELECT * FROM rooms WHERE room_id=?", (room_id,)).fetchone()
        if existing is None:
            raise HTTPException(status_code=404, detail="房间不存在")
        try:
            current_limits = json.loads(existing["seat_limits"])
            current_parameters = json.loads(existing["seat_parameters"])
        except (TypeError, json.JSONDecodeError) as exc:
            raise HTTPException(status_code=503, detail="房间配置数据损坏") from exc
        seat_limits = dict(DEFAULT_SEAT_LIMITS)
        if isinstance(current_limits, dict):
            seat_limits.update(current_limits)
        if "seat_limits" in body.model_fields_set:
            seat_limits.update(body.seat_limits)
        seat_parameters = (
            body.seat_parameters
            if "seat_parameters" in body.model_fields_set
            else current_parameters
        )
        intel_stale_after_sec = (
            body.intel_stale_after_sec
            if "intel_stale_after_sec" in body.model_fields_set
            else float(existing["intel_stale_after_sec"])
        )
        intel_archive_after_sec = (
            body.intel_archive_after_sec
            if "intel_archive_after_sec" in body.model_fields_set
            else float(existing["intel_archive_after_sec"])
        )
        protocol_profile = (
            body.protocol_profile
            if "protocol_profile" in body.model_fields_set
            else existing["protocol_profile"]
        )
        if intel_archive_after_sec <= intel_stale_after_sec:
            raise HTTPException(status_code=422, detail="情报归档阈值必须大于失联阈值")
        mode_changed = "mode" in body.model_fields_set and body.mode != existing["mode"]
        difficulty_changed = (
            "ai_difficulty" in body.model_fields_set
            and body.ai_difficulty != existing["ai_difficulty"]
        )
        if existing["status"] == "running":
            raise HTTPException(status_code=409, detail="推演进行中不能修改房间配置")
        if (mode_changed or difficulty_changed) and existing["status"] != "stopped":
            raise HTTPException(status_code=409, detail="只有停止状态才能修改房间模式或 AI 强度")
        mode = body.mode if "mode" in body.model_fields_set else existing["mode"]
        ai_difficulty = (
            body.ai_difficulty
            if "ai_difficulty" in body.model_fields_set
            else existing["ai_difficulty"]
        )
        provider = (
            body.ai_provider
            if "ai_provider" in body.model_fields_set and body.ai_provider is not None
            else room_ai_provider(existing["ai_provider"], existing["mode"])
        )
        model = (
            body.ai_model
            if "ai_model" in body.model_fields_set and body.ai_model is not None
            else room_ai_model(existing["ai_model"])
        )
        ai_changed = provider != room_ai_provider(existing["ai_provider"], existing["mode"])
        ai_changed = ai_changed or model != room_ai_model(existing["ai_model"])
        if ai_changed and existing["status"] != "stopped":
            raise HTTPException(status_code=409, detail="只有停止状态才能修改房间 AI 配置")
        if mode == "pvp" and "ai_provider" not in body.model_fields_set:
            provider = "rules"
            model = ""
        if mode == "pvp":
            provider = "rules"
            if "ai_model" not in body.model_fields_set:
                model = ""
        config_version = int(existing["config_version"])
        changed = (
            body.name != existing["name"]
            or body.description != existing["description"]
            or body.scenario_id != existing["scenario_id"]
            or seat_limits != current_limits
            or seat_parameters != current_parameters
            or bool(body.enabled) != bool(existing["enabled"])
            or mode != existing["mode"]
            or ai_difficulty != existing["ai_difficulty"]
            or provider != room_ai_provider(existing["ai_provider"], existing["mode"])
            or model != room_ai_model(existing["ai_model"])
            or intel_stale_after_sec != float(existing["intel_stale_after_sec"])
            or intel_archive_after_sec != float(existing["intel_archive_after_sec"])
            or protocol_profile != existing["protocol_profile"]
        )
        if changed:
            config_version += 1
        now = iso_time(utc_now())
        db.execute(
            "UPDATE rooms SET name=?,description=?,scenario_id=?,protocol_profile=?,seat_limits=?,seat_parameters=?,mode=?,ai_difficulty=?,ai_provider=?,ai_model=?,ai_resolved_model=?,config_version=?,intel_stale_after_sec=?,intel_archive_after_sec=?,enabled=?,updated_at=? WHERE room_id=?",
            (body.name, body.description, body.scenario_id, protocol_profile,
             json.dumps(seat_limits, ensure_ascii=False),
             json.dumps(seat_parameters, ensure_ascii=False), mode, ai_difficulty,
             provider, model, "" if ai_changed or mode_changed else existing["ai_resolved_model"],
             config_version, intel_stale_after_sec, intel_archive_after_sec,
             int(body.enabled), now, room_id),
        )
        row = db.execute("SELECT * FROM rooms WHERE room_id=?", (room_id,)).fetchone()
    return {"room": public_room(row)}


@app.delete("/api/admin/rooms/{room_id}")
def delete_room(room_id: str, _: sqlite3.Row = Depends(require_admin)) -> dict:
    if room_id == ACTIVE_GAME_ROOM_ID:
        raise HTTPException(status_code=409, detail="当前受托管房间不能删除")
    with database() as db:
        existing = db.execute("SELECT status FROM rooms WHERE room_id=?", (room_id,)).fetchone()
        if existing is None:
            raise HTTPException(status_code=404, detail="房间不存在")
        if existing["status"] == "running":
            raise HTTPException(status_code=409, detail="推演进行中不能删除房间")
        for table in ("room_operations", "room_presence", "room_kick_requests", "room_audit"):
            db.execute(f"DELETE FROM {table} WHERE room_id=?", (room_id,))
        db.execute("DELETE FROM rooms WHERE room_id=?", (room_id,))
    return {"deleted": True}


@app.post("/api/admin/rooms/{room_id}/{action}")
def room_action(room_id: str, action: str, _: sqlite3.Row = Depends(require_admin)) -> dict:
    actions = {"open": "preparing", "start": "running", "resume": "running", "finish": "finished",
               "stop": "stopped", "pause": "paused", "reset": "preparing",
               "redeploy": "preparing", "force-stop": "stopped"}
    if action not in actions:
        raise HTTPException(status_code=422, detail="不支持的房间操作")
    if room_id != ACTIVE_GAME_ROOM_ID:
        raise HTTPException(
            status_code=409,
            detail=f"当前游戏服务仅托管 {ACTIVE_GAME_ROOM_ID} 房间，不能执行生命周期操作",
        )
    with database() as db:
        row = db.execute("SELECT * FROM rooms WHERE room_id=?", (room_id,)).fetchone()
        if row is None:
            raise HTTPException(status_code=404, detail="房间不存在")
        pending = db.execute(
            "SELECT operation_id, requested_at FROM room_operations WHERE room_id=? AND state='pending' "
            "ORDER BY requested_at DESC, operation_id DESC LIMIT 1",
            (room_id,),
        ).fetchone()
        now_value = utc_now()
        now = iso_time(now_value)
        if pending is not None:
            if action == "force-stop" or (action == "pause" and pending_operation_timed_out(pending, now_value)):
                db.execute(
                    "UPDATE room_operations SET state='failed', acknowledged_at=? WHERE operation_id=?",
                    (now, pending["operation_id"]),
                )
            else:
                raise HTTPException(status_code=409, detail="上一项房间操作尚未得到兵棋服务确认")
        if action == "open" and row["status"] not in {"stopped", "finished"}:
            raise HTTPException(status_code=409, detail="只有停止或已结束的房间才能进入准备阶段")
        if action == "force-stop" and row["status"] == "stopped":
            raise HTTPException(status_code=409, detail="房间已经停止")
        if action == "start" and row["status"] != "preparing":
            raise HTTPException(status_code=409, detail="房间尚未进入准备阶段")
        if action == "start":
            if not room_ready_for_start(room_id):
                raise HTTPException(status_code=409, detail="所有已占用战位必须完成部署并就绪，不能开始推演")
        if action == "resume" and row["status"] != "paused":
            raise HTTPException(status_code=409, detail="当前房间未暂停，不能继续推演")
        if action == "finish" and row["status"] not in {"running", "preparing", "paused"}:
            raise HTTPException(status_code=409, detail="当前房间状态不能结束推演")
        if action == "pause" and row["status"] not in {"preparing", "running", "paused"}:
            raise HTTPException(status_code=409, detail="当前房间状态不能暂停")
        operation_id = secrets.token_urlsafe(18)
        # Every lifecycle transition is a two-phase operation.  The account
        # service records intent; only the authoritative game server may
        # commit the target status through the internal acknowledgement API.
        operation_state = "pending"
        requested_revision = room_lifecycle_revision(room_id)
        requested_config_version = int(row["config_version"])
        global_config = db.execute(
            "SELECT config_version FROM ai_config WHERE config_id=1"
        ).fetchone()
        requested_ollama_config_version = int(global_config["config_version"]) if global_config else 1
        if action in {"reset", "redeploy"} and requested_revision <= 0:
            raise HTTPException(status_code=409, detail="兵棋服务尚未发布可操作的房间版本")
        try:
            db.execute(
                "INSERT INTO room_operations(operation_id,room_id,action,expected_status,state,requested_revision,requested_config_version,requested_ollama_config_version,requested_at,acknowledged_at) "
                "VALUES(?,?,?,?,?,?,?,?,?,?)",
                (operation_id, room_id, action, actions[action], operation_state, requested_revision,
                 requested_config_version, requested_ollama_config_version, now, None),
            )
        except sqlite3.IntegrityError as exc:
            raise HTTPException(status_code=409, detail="上一项房间操作尚未得到兵棋服务确认") from exc
        updated = db.execute("SELECT * FROM rooms WHERE room_id=?", (room_id,)).fetchone()
    return {
        "room": public_room(updated),
        "operation": {
            "operationId": operation_id,
            "action": action,
            "expectedStatus": actions[action],
            "state": operation_state,
            "requestedRevision": requested_revision,
            "requestedConfigVersion": requested_config_version,
            "requestedOllamaConfigVersion": requested_ollama_config_version,
            "appliedRevision": None,
            "code": "",
            "requestedAt": now,
            "acknowledgedAt": None,
        },
    }


@app.put("/api/admin/password")
def change_admin_password(body: PasswordBody, admin: sqlite3.Row = Depends(require_admin)) -> dict:
    if not verify_password(admin["password_hash"], body.current_password):
        raise HTTPException(status_code=403, detail="当前管理员密码错误")
    with database() as db:
        db.execute(
            "UPDATE admins SET password_hash=?, updated_at=? WHERE id=?",
            (password_hasher.hash(body.new_password), iso_time(utc_now()), admin["id"]),
        )
        db.execute("DELETE FROM sessions WHERE subject_type='admin' AND subject_id=?", (admin["id"],))
    invalidate_terminal_tickets()
    return {"changed": True, "message": "密码已修改，请重新登录"}


@app.post("/api/admin/logout")
def admin_logout(authorization: str | None = Header(default=None)) -> dict:
    token = bearer_token(authorization)
    digest = token_digest(token)
    with database() as db:
        db.execute("DELETE FROM sessions WHERE token_hash=?", (digest,))
    invalidate_terminal_tickets(digest)
    return {"loggedOut": True}


@app.post("/api/client/logout")
def client_logout(authorization: str | None = Header(default=None)) -> dict:
    token = bearer_token(authorization)
    with database() as db:
        session = db.execute(
            "SELECT subject_id FROM sessions WHERE token_hash=? AND subject_type='player'",
            (token_digest(token),),
        ).fetchone()
        db.execute(
            "DELETE FROM sessions WHERE token_hash=? AND subject_type='player'",
            (token_digest(token),),
        )
        if session is not None:
            admin = db.execute("SELECT id FROM admins ORDER BY id LIMIT 1").fetchone()
            if admin is not None:
                queue_user_invalidation(db, session["subject_id"], "用户已退出登录", admin["id"])
    return {"loggedOut": True}


@app.post("/api/internal/session")
def internal_session(
    body: TokenBody,
    x_internal_key: str | None = Header(default=None),
) -> dict:
    if not secrets.compare_digest(x_internal_key or "", INTERNAL_KEY):
        raise HTTPException(status_code=403, detail="内部认证失败")
    with database() as db:
        row = db.execute(
            "SELECT u.id, u.username, u.display_name, u.role, u.enabled, s.expires_at "
            "FROM sessions s JOIN users u ON u.id=s.subject_id "
            "WHERE s.token_hash=? AND s.subject_type='player' AND s.expires_at>?",
            (token_digest(body.token), iso_time(utc_now())),
        ).fetchone()
    if row is None or not bool(row["enabled"]):
        raise HTTPException(status_code=401, detail="登录会话无效")
    return {
        "valid": True,
        "userId": row["id"],
        "username": row["username"],
        "displayName": normalized_display_name(row["username"], row["display_name"]),
        "role": normalize_user_role(row["role"]),
        "expiresAt": row["expires_at"],
    }


@app.get("/api/internal/rooms")
def internal_rooms(x_internal_key: str | None = Header(default=None)) -> dict:
    if not secrets.compare_digest(x_internal_key or "", INTERNAL_KEY):
        raise HTTPException(status_code=403, detail="内部认证失败")
    with database() as db:
        rows = room_rows(db, True)
        ai_config_row = db.execute("SELECT * FROM ai_config WHERE config_id=1").fetchone()
        kicks = db.execute(
            "SELECT id, room_id, user_id, reason, requested_at FROM room_kick_requests "
            "WHERE processed_at IS NULL ORDER BY requested_at LIMIT 128"
        ).fetchall()
        logout_requests = db.execute(
            "SELECT id, user_id, reason, requested_at FROM user_kick_requests "
            "WHERE processed_at IS NULL ORDER BY requested_at LIMIT 128"
        ).fetchall()
    return {
        "rooms": [public_room(row) for row in rows],
        "aiConfig": public_ai_config(ai_config_row) if ai_config_row is not None else {},
        "kickRequests": [{"id": row["id"], "roomId": row["room_id"], "userId": row["user_id"],
                          "reason": row["reason"], "requestedAt": row["requested_at"]} for row in kicks],
        "logoutRequests": [{"id": row["id"], "userId": row["user_id"], "reason": row["reason"],
                            "requestedAt": row["requested_at"]} for row in logout_requests],
    }


@app.put("/api/internal/rooms/{room_id}/config")
def internal_room_config(
    room_id: str,
    body: InternalRoomConfigBody,
    x_internal_key: str | None = Header(default=None),
) -> dict:
    require_internal_key(x_internal_key)
    with database() as db:
        existing = db.execute("SELECT * FROM rooms WHERE room_id=?", (room_id,)).fetchone()
        if existing is None:
            raise HTTPException(status_code=404, detail="房间不存在")
        if int(existing["config_version"]) != body.expected_config_version:
            raise HTTPException(status_code=409, detail="ROOM_CONFIG_VERSION_CONFLICT")
        if existing["status"] != "preparing":
            raise HTTPException(status_code=409, detail="只有准备阶段可以编辑房间配置")
        try:
            current_limits = json.loads(existing["seat_limits"])
            current_parameters = json.loads(existing["seat_parameters"])
        except (TypeError, json.JSONDecodeError) as exc:
            raise HTTPException(status_code=503, detail="房间配置数据损坏") from exc
        seat_limits = dict(DEFAULT_SEAT_LIMITS)
        if isinstance(current_limits, dict):
            seat_limits.update(current_limits)
        if "seat_limits" in body.model_fields_set:
            seat_limits.update(body.seat_limits)
        seat_parameters = (
            body.seat_parameters
            if "seat_parameters" in body.model_fields_set
            else current_parameters
        )
        protocol_profile = (
            body.protocol_profile
            if "protocol_profile" in body.model_fields_set
            else existing["protocol_profile"]
        )
        changed = (
            body.name != existing["name"]
            or body.description != existing["description"]
            or body.scenario_id != existing["scenario_id"]
            or seat_limits != current_limits
            or seat_parameters != current_parameters
            or protocol_profile != existing["protocol_profile"]
        )
        next_version = int(existing["config_version"]) + (1 if changed else 0)
        now = iso_time(utc_now())
        update = db.execute(
            "UPDATE rooms SET name=?,description=?,scenario_id=?,protocol_profile=?,seat_limits=?,seat_parameters=?,config_version=?,updated_at=? "
            "WHERE room_id=? AND config_version=?",
            (body.name, body.description, body.scenario_id, protocol_profile,
             json.dumps(seat_limits, ensure_ascii=False),
             json.dumps(seat_parameters, ensure_ascii=False), next_version, now,
             room_id, body.expected_config_version),
        )
        if update.rowcount != 1:
            raise HTTPException(status_code=409, detail="ROOM_CONFIG_VERSION_CONFLICT")
        row = db.execute("SELECT * FROM rooms WHERE room_id=?", (room_id,)).fetchone()
    return {"room": public_room(row)}


class InternalKickAckBody(BaseModel):
    request_id: int = Field(gt=0)
    result: Literal["kicked", "not_found", "ignored"] = "ignored"


@app.post("/api/internal/rooms/{room_id}/kick-requests/{request_id}/ack")
def acknowledge_kick_request(
    room_id: str,
    request_id: int,
    body: InternalKickAckBody,
    x_internal_key: str | None = Header(default=None),
) -> dict:
    if not secrets.compare_digest(x_internal_key or "", INTERNAL_KEY):
        raise HTTPException(status_code=403, detail="内部认证失败")
    with database() as db:
        row = db.execute(
            "SELECT id FROM room_kick_requests WHERE id=? AND room_id=? AND processed_at IS NULL",
            (request_id, room_id),
        ).fetchone()
        if row is None:
            return {"accepted": True, "alreadyProcessed": True}
        db.execute(
            "UPDATE room_kick_requests SET processed_at=? WHERE id=?",
            (iso_time(utc_now()), request_id),
        )
        db.execute(
            "INSERT INTO room_audit(room_id,user_id,admin_id,action,detail,created_at) "
            "SELECT room_id,user_id,requested_by,?,reason,? FROM room_kick_requests WHERE id=?",
            (f"kick_{body.result}", iso_time(utc_now()), request_id),
        )
    return {"accepted": True}


class InternalLogoutAckBody(BaseModel):
    request_id: int = Field(gt=0)
    result: Literal["kicked", "not_found", "ignored"] = "ignored"


@app.post("/api/internal/logout-requests/{request_id}/ack")
def acknowledge_logout_request(
    request_id: int,
    body: InternalLogoutAckBody,
    x_internal_key: str | None = Header(default=None),
) -> dict:
    if not secrets.compare_digest(x_internal_key or "", INTERNAL_KEY):
        raise HTTPException(status_code=403, detail="内部认证失败")
    with database() as db:
        row = db.execute(
            "SELECT id FROM user_kick_requests WHERE id=? AND processed_at IS NULL", (request_id,)
        ).fetchone()
        if row is None:
            return {"accepted": True, "alreadyProcessed": True}
        db.execute(
            "UPDATE user_kick_requests SET processed_at=? WHERE id=?",
            (iso_time(utc_now()), request_id),
        )
        db.execute(
            "INSERT INTO terminal_audit(admin_id,action,created_at) "
            "SELECT requested_by,?,? FROM user_kick_requests WHERE id=?",
            (f"user_kick_{body.result}:{request_id}", iso_time(utc_now()), request_id),
        )
    return {"accepted": True}


@app.post("/api/internal/rooms/{room_id}/status")
def internal_room_status(
    room_id: str,
    body: InternalRoomStatusBody,
    x_internal_key: str | None = Header(default=None),
) -> dict:
    """Allow game-server readiness checks to roll back an invalid admin start.

    This is deliberately narrower than the admin room action API: it cannot
    change room configuration, only reconcile the lifecycle status observed by
    the authoritative game process.
    """
    if not secrets.compare_digest(x_internal_key or "", INTERNAL_KEY):
        raise HTTPException(status_code=403, detail="内部认证失败")
    with database() as db:
        existing = db.execute("SELECT * FROM rooms WHERE room_id=?", (room_id,)).fetchone()
        if existing is None:
            raise HTTPException(status_code=404, detail="房间不存在")
        now = iso_time(utc_now())
        winner = body.winner if body.status == "finished" else ""
        pending = db.execute(
            "SELECT operation_id, action, expected_status FROM room_operations "
            "WHERE room_id=? AND state='pending' ORDER BY requested_at DESC, operation_id DESC LIMIT 1",
            (room_id,),
        ).fetchone()
        if pending is not None:
            # A lifecycle request is committed only by the operation ACK.  A
            # status heartbeat cannot race that transaction or turn a failed
            # validation into a successful transition.
            sync_presence(db, room_id, body.occupants)
            current = next(item for item in room_rows(db, False) if item["room_id"] == room_id)
            return {"room": public_room(current), "reason": "pending operation"}
        db.execute(
            "UPDATE rooms SET status=?,winner=?,status_reason=?,updated_at=? WHERE room_id=?",
            (body.status, winner, body.reason, now, room_id),
        )
        sync_presence(db, room_id, body.occupants)
        updated = db.execute("SELECT * FROM rooms WHERE room_id=?", (room_id,)).fetchone()
    return {"room": public_room(updated), "reason": body.reason}


@app.post("/api/internal/rooms/{room_id}/operations/{operation_id}/ack")
def internal_room_operation_ack(
    room_id: str,
    operation_id: str,
    body: InternalOperationAckBody,
    x_internal_key: str | None = Header(default=None),
) -> dict:
    if not secrets.compare_digest(x_internal_key or "", INTERNAL_KEY):
        raise HTTPException(status_code=403, detail="内部认证失败")
    with database() as db:
        row = db.execute(
            "SELECT operation_id,action,expected_status,state,requested_revision,applied_revision,result_code "
            "FROM room_operations WHERE operation_id=? AND room_id=?",
            (operation_id, room_id),
        ).fetchone()
        if row is None:
            raise HTTPException(status_code=404, detail="房间操作不存在")
        if row["state"] != "pending":
            if (row["state"] == body.state
                    and (row["applied_revision"] or 0) == body.revision
                    and row["result_code"] == body.code):
                updated = next(item for item in room_rows(db, False) if item["room_id"] == room_id)
                return {"accepted": True, "alreadyProcessed": True,
                        "room": public_room(updated), "revision": body.revision,
                        "code": body.code}
            raise HTTPException(status_code=409, detail="房间操作已以不同结果完成")
        if body.state == "acknowledged" and body.revision <= row["requested_revision"]:
            raise HTTPException(status_code=409, detail="房间操作确认版本与请求版本不连续")
        now = iso_time(utc_now())
        db.execute(
            "UPDATE room_operations SET state=?,applied_revision=?,result_code=?,acknowledged_at=? "
            "WHERE operation_id=? AND state='pending'",
            (body.state, body.revision, body.code, now, operation_id),
        )
        if body.state == "acknowledged":
            db.execute(
                "UPDATE rooms SET status=?,winner='',status_reason='',ai_resolved_model=?,updated_at=? WHERE room_id=?",
                (row["expected_status"], body.ai_resolved_model.strip(), now, room_id),
            )
        elif row["action"] == "open":
            # A failed open/configuration probe must leave a stopped room and
            # must never publish a partially applied model.
            db.execute(
                "UPDATE rooms SET status='stopped',winner='',ai_resolved_model='',status_reason=?,updated_at=? WHERE room_id=?",
                (body.code or "房间配置校验失败", now, room_id),
            )
        updated = next(item for item in room_rows(db, False) if item["room_id"] == room_id)
    return {"accepted": True, "room": public_room(updated), "revision": body.revision,
            "code": body.code}


@app.post("/api/internal/rooms/{room_id}/presence")
def internal_room_presence(
    room_id: str,
    body: InternalPresenceBody,
    x_internal_key: str | None = Header(default=None),
) -> dict:
    if not secrets.compare_digest(x_internal_key or "", INTERNAL_KEY):
        raise HTTPException(status_code=403, detail="内部认证失败")
    with database() as db:
        existing = db.execute("SELECT room_id FROM rooms WHERE room_id=?", (room_id,)).fetchone()
        if existing is None:
            raise HTTPException(status_code=404, detail="房间不存在")
        sync_presence(db, room_id, body.occupants)
    return {"accepted": True, "occupantCount": len(body.occupants)}
