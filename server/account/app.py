from __future__ import annotations

import asyncio
import errno
import hashlib
import json
import os
import fcntl
import pty
import secrets
import sqlite3
import threading
import time
import termios
import subprocess
from contextlib import contextmanager
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Iterator, Literal

from argon2 import PasswordHasher
from argon2.exceptions import VerificationError
from fastapi import Depends, FastAPI, Header, HTTPException, Request, WebSocket, WebSocketDisconnect, status
from fastapi.responses import FileResponse
from pydantic import BaseModel, Field, field_validator
from starlette.websockets import WebSocketState


DATA_DIR = Path(os.getenv("DATA_DIR", "/data"))
DB_PATH = DATA_DIR / "wargame.db"
STATIC_DIR = Path(__file__).resolve().parent / "static"
SESSION_HOURS = int(os.getenv("SESSION_HOURS", "12"))
INTERNAL_KEY = os.getenv("INTERNAL_API_KEY", "").strip()
PUBLIC_WS_URL = os.getenv("PUBLIC_GAME_WS_URL", "ws://localhost:8090")
GAME_STATUS_PATH = Path(os.getenv("GAME_STATUS_PATH", "/data/game-status.json"))
GAME_EVENTS_PATH = Path(os.getenv("GAME_EVENTS_PATH", "/data/game-events.jsonl"))
WEB_SHELL_ENABLED = os.getenv("WEB_SHELL_ENABLED", "false").strip().lower() == "true"
WEB_SHELL_TICKET_SECONDS = max(30, min(int(os.getenv("WEB_SHELL_TICKET_SECONDS", "120")), 600))
WEB_SHELL_SESSION_SECONDS = max(60, min(int(os.getenv("WEB_SHELL_SESSION_SECONDS", "900")), 3600))
WEB_SHELL_MAX_SESSIONS = max(1, min(int(os.getenv("WEB_SHELL_MAX_SESSIONS", "2")), 8))
ROLES = {"director", "editor", "red", "blue"}

password_hasher = PasswordHasher(time_cost=3, memory_cost=65536, parallelism=2)
login_attempts: dict[str, list[float]] = {}
login_attempts_lock = threading.Lock()
terminal_tickets: dict[str, tuple[int, str, float]] = {}
terminal_tickets_lock = threading.Lock()
active_terminal_sessions = 0
active_terminal_sessions_lock = threading.Lock()

app = FastAPI(
    title="兵器推演账号中心",
    docs_url=None,
    redoc_url=None,
    openapi_url=None,
)


class LoginBody(BaseModel):
    username: str = Field(min_length=1, max_length=64)
    password: str = Field(min_length=1, max_length=256)

    @field_validator("username", mode="before")
    @classmethod
    def strip_username(cls, value: object) -> object:
        return value.strip() if isinstance(value, str) else value


class UserBody(BaseModel):
    username: str = Field(min_length=3, max_length=64, pattern=r"^[A-Za-z0-9_.-]+$")
    display_name: str = Field(min_length=1, max_length=64)
    role: Literal["director", "editor", "red", "blue"]
    password: str | None = Field(default=None, max_length=256)
    enabled: bool = True

    @field_validator("username", "display_name", mode="before")
    @classmethod
    def strip_text(cls, value: object) -> object:
        return value.strip() if isinstance(value, str) else value


class PasswordBody(BaseModel):
    current_password: str = Field(min_length=1, max_length=256)
    new_password: str = Field(min_length=8, max_length=256)


class TokenBody(BaseModel):
    token: str = Field(min_length=20, max_length=256)


class TerminalLoginBody(BaseModel):
    password: str = Field(min_length=1, max_length=256)


def utc_now() -> datetime:
    return datetime.now(timezone.utc)


def iso_time(value: datetime) -> str:
    return value.isoformat(timespec="seconds").replace("+00:00", "Z")


def token_digest(token: str) -> str:
    return hashlib.sha256(token.encode("utf-8")).hexdigest()


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
                role TEXT NOT NULL CHECK(role IN ('director', 'editor', 'red', 'blue')),
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
            CREATE TABLE IF NOT EXISTS terminal_audit (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                admin_id INTEGER NOT NULL,
                action TEXT NOT NULL,
                created_at TEXT NOT NULL,
                FOREIGN KEY(admin_id) REFERENCES admins(id)
            );
            CREATE INDEX IF NOT EXISTS idx_terminal_audit_admin_time
                ON terminal_audit(admin_id, created_at);
            """
        )
        username = os.getenv("ADMIN_USERNAME", "admin").strip() or "admin"
        password = os.getenv("ADMIN_PASSWORD", "")
        existing = db.execute("SELECT id FROM admins LIMIT 1").fetchone()
        if existing is None:
            if len(password) < 8 or password in {"Admin123456!", "change-me"}:
                raise RuntimeError("首次启动必须通过 ADMIN_PASSWORD 设置至少 8 位的管理员密码")
            db.execute(
                "INSERT INTO admins(username, password_hash, updated_at) VALUES(?, ?, ?)",
                (username, password_hasher.hash(password), iso_time(utc_now())),
            )


@app.on_event("startup")
def on_startup() -> None:
    if len(INTERNAL_KEY) < 32 or INTERNAL_KEY == "change-this-internal-key":
        raise RuntimeError("INTERNAL_API_KEY 必须是至少 32 位的随机密钥")
    initialize_database()


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


def enforce_login_limit(key: str) -> None:
    now = time.monotonic()
    with login_attempts_lock:
        attempts = [value for value in login_attempts.get(key, []) if now - value < 60]
        if attempts:
            login_attempts[key] = attempts
        else:
            login_attempts.pop(key, None)
        limited = len(attempts) >= 8
    if limited:
        raise HTTPException(status_code=429, detail="登录尝试过于频繁，请稍后再试")


def record_login_failure(key: str) -> None:
    now = time.monotonic()
    with login_attempts_lock:
        # Periodic pruning prevents attempts against random usernames from
        # growing this process-local limiter without bound.
        for candidate in list(login_attempts):
            recent = [value for value in login_attempts[candidate] if now - value < 60]
            if recent:
                login_attempts[candidate] = recent
            else:
                del login_attempts[candidate]
        login_attempts.setdefault(key, []).append(now)


def clear_login_failures(key: str) -> None:
    with login_attempts_lock:
        login_attempts.pop(key, None)


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


def admin_session_is_valid(admin_id: int, session_digest: str) -> bool:
    with database() as db:
        row = db.execute(
            "SELECT 1 FROM sessions WHERE token_hash=? AND subject_type='admin' "
            "AND subject_id=? AND expires_at>?",
            (session_digest, admin_id, iso_time(utc_now())),
        ).fetchone()
    return row is not None


def record_terminal_audit(admin_id: int, action: str) -> None:
    with database() as db:
        db.execute(
            "INSERT INTO terminal_audit(admin_id, action, created_at) VALUES(?, ?, ?)",
            (admin_id, action, iso_time(utc_now())),
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
    return {
        "id": row["id"],
        "username": row["username"],
        "displayName": row["display_name"],
        "role": row["role"],
        "enabled": bool(row["enabled"]),
        "createdAt": row["created_at"],
        "updatedAt": row["updated_at"],
    }


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
    return {"status": "ok", "service": "account-web"}


@app.post("/api/admin/login")
def admin_login(body: LoginBody, request: Request) -> dict:
    limit_key = f"admin:{request.client.host if request.client else 'local'}:{body.username}"
    enforce_login_limit(limit_key)
    with database() as db:
        row = db.execute(
            "SELECT id, username, password_hash FROM admins WHERE username = ?",
            (body.username.strip(),),
        ).fetchone()
        if row is None or not verify_password(row["password_hash"], body.password):
            record_login_failure(limit_key)
            raise HTTPException(status_code=401, detail="管理员用户名或密码错误")
        token = create_session(db, "admin", row["id"])
    clear_login_failures(limit_key)
    return {"token": token, "username": row["username"], "expiresInHours": SESSION_HOURS}


@app.post("/api/client/login")
def client_login(body: LoginBody, request: Request) -> dict:
    limit_key = f"player:{request.client.host if request.client else 'local'}:{body.username.casefold()}"
    enforce_login_limit(limit_key)
    with database() as db:
        row = db.execute(
            "SELECT * FROM users WHERE username = ? COLLATE NOCASE",
            (body.username.strip(),),
        ).fetchone()
        if row is None or not bool(row["enabled"]) or not verify_password(row["password_hash"], body.password):
            record_login_failure(limit_key)
            raise HTTPException(status_code=401, detail="用户名或密码错误，或账号已停用")
        token = create_session(db, "player", row["id"])
    clear_login_failures(limit_key)
    return {
        "token": token,
        "username": row["username"],
        "displayName": row["display_name"],
        "role": row["role"],
        "gameWebSocketUrl": PUBLIC_WS_URL,
        "expiresInHours": SESSION_HOURS,
    }


@app.get("/api/admin/me")
def admin_me(admin: sqlite3.Row = Depends(require_admin)) -> dict:
    return {"username": admin["username"]}


@app.get("/api/admin/users")
def list_users(_: sqlite3.Row = Depends(require_admin)) -> dict:
    with database() as db:
        rows = db.execute("SELECT * FROM users ORDER BY role, username COLLATE NOCASE").fetchall()
    return {"users": [public_user(row) for row in rows]}


@app.get("/api/admin/monitor/overview")
def monitor_overview(_: sqlite3.Row = Depends(require_admin)) -> dict:
    game_status = read_game_status()
    with database() as db:
        active_players = db.execute(
            "SELECT COUNT(*) FROM sessions WHERE subject_type='player' AND expires_at > ?",
            (iso_time(utc_now()),),
        ).fetchone()[0]
    return {
        "accountStatus": "healthy",
        "gameStatus": game_status or {"status": "unknown"},
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


@app.post("/api/admin/monitor/terminal/login")
def terminal_login(
    body: TerminalLoginBody,
    authorization: str | None = Header(default=None),
    admin: sqlite3.Row = Depends(require_admin),
) -> dict:
    if not WEB_SHELL_ENABLED:
        raise HTTPException(status_code=503, detail="网页 Shell 未在此服务实例启用")
    if not verify_password(admin["password_hash"], body.password):
        raise HTTPException(status_code=403, detail="管理员密码错误")
    session_digest = token_digest(bearer_token(authorization))
    ticket = issue_terminal_ticket(admin["id"], session_digest)
    record_terminal_audit(admin["id"], "authorized")
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
        await websocket.close(code=status.WS_1008_POLICY_VIOLATION)
        return

    identity = consume_terminal_ticket(websocket.query_params.get("ticket"))
    if identity is None:
        await websocket.close(code=status.WS_1008_POLICY_VIOLATION)
        return
    admin_id, session_digest = identity
    if not admin_session_is_valid(admin_id, session_digest):
        await websocket.close(code=status.WS_1008_POLICY_VIOLATION)
        return
    if not reserve_terminal_session():
        await websocket.close(code=status.WS_1013_TRY_AGAIN_LATER)
        return

    master_fd: int | None = None
    slave_fd: int | None = None
    process: subprocess.Popen[bytes] | None = None
    try:
        await websocket.accept()
        master_fd, slave_fd = pty.openpty()
        flags = fcntl.fcntl(master_fd, fcntl.F_GETFL)
        fcntl.fcntl(master_fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)
        process = subprocess.Popen(
            ["/bin/sh", "-i"],
            stdin=slave_fd,
            stdout=slave_fd,
            stderr=slave_fd,
            cwd="/app",
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
            try:
                message = await asyncio.wait_for(websocket.receive_text(), timeout=0.05)
            except TimeoutError:
                message = None
            except WebSocketDisconnect:
                break

            if message is not None:
                if len(message.encode("utf-8")) > 4096:
                    await websocket.send_text("\r\n单次输入不能超过 4096 字节。\r\n")
                    continue
                if not admin_session_is_valid(admin_id, session_digest):
                    await websocket.send_text("\r\n管理员会话已失效，终端已关闭。\r\n")
                    break
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
        pass
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
        record_terminal_audit(admin_id, "closed")
        release_terminal_session()


@app.post("/api/admin/users", status_code=201)
def create_user(body: UserBody, _: sqlite3.Row = Depends(require_admin)) -> dict:
    if not body.password or len(body.password) < 8:
        raise HTTPException(status_code=422, detail="新账号密码至少需要 8 个字符")
    now = iso_time(utc_now())
    try:
        with database() as db:
            cursor = db.execute(
                "INSERT INTO users(username, display_name, role, password_hash, enabled, created_at, updated_at) "
                "VALUES(?, ?, ?, ?, ?, ?, ?)",
                (
                    body.username,
                    body.display_name,
                    body.role,
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
def update_user(user_id: int, body: UserBody, _: sqlite3.Row = Depends(require_admin)) -> dict:
    try:
        with database() as db:
            existing = db.execute("SELECT * FROM users WHERE id = ?", (user_id,)).fetchone()
            if existing is None:
                raise HTTPException(status_code=404, detail="账号不存在")
            password_hash = existing["password_hash"]
            if body.password:
                if len(body.password) < 8:
                    raise HTTPException(status_code=422, detail="密码至少需要 8 个字符")
                password_hash = password_hasher.hash(body.password)
            db.execute(
                "UPDATE users SET username=?, display_name=?, role=?, password_hash=?, enabled=?, updated_at=? "
                "WHERE id=?",
                (
                    body.username,
                    body.display_name,
                    body.role,
                    password_hash,
                    int(body.enabled),
                    iso_time(utc_now()),
                    user_id,
                ),
            )
            if (
                body.password
                or not body.enabled
                or body.role != existing["role"]
                or body.username.casefold() != existing["username"].casefold()
                or body.display_name != existing["display_name"]
            ):
                db.execute("DELETE FROM sessions WHERE subject_type='player' AND subject_id=?", (user_id,))
            row = db.execute("SELECT * FROM users WHERE id = ?", (user_id,)).fetchone()
    except sqlite3.IntegrityError as exc:
        raise HTTPException(status_code=409, detail="用户名已存在") from exc
    return {"user": public_user(row)}


@app.delete("/api/admin/users/{user_id}")
def delete_user(user_id: int, _: sqlite3.Row = Depends(require_admin)) -> dict:
    with database() as db:
        existing = db.execute("SELECT id FROM users WHERE id = ?", (user_id,)).fetchone()
        if existing is None:
            raise HTTPException(status_code=404, detail="账号不存在")
        db.execute("DELETE FROM sessions WHERE subject_type='player' AND subject_id=?", (user_id,))
        db.execute("DELETE FROM users WHERE id=?", (user_id,))
    return {"deleted": True}


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
        db.execute(
            "DELETE FROM sessions WHERE token_hash=? AND subject_type='player'",
            (token_digest(token),),
        )
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
        "displayName": row["display_name"],
        "role": row["role"],
        "expiresAt": row["expires_at"],
    }
