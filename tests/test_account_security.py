from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest

from fastapi import HTTPException
from pydantic import ValidationError
from starlette.requests import Request


APP_PATH = Path(__file__).resolve().parents[1] / "server" / "account" / "app.py"


class AccountSecurityTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        os.environ["ADMIN_PASSWORD"] = "isolated-test-password"
        os.environ["INTERNAL_API_KEY"] = "i" * 32
        os.environ["WEB_SHELL_ALLOWED_ORIGINS"] = (
            "http://127.0.0.1:8080,http://localhost:8080"
        )
        spec = importlib.util.spec_from_file_location("account_app_security", APP_PATH)
        assert spec is not None and spec.loader is not None
        cls.app = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = cls.app
        spec.loader.exec_module(cls.app)

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        root = Path(self.temporary.name)
        self.app.DATA_DIR = root
        self.app.DB_PATH = root / "wargame.db"
        self.app.initialize_database()
        self.app.terminal_tickets.clear()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    @staticmethod
    def request(ip_address: str = "127.0.0.1", forwarded_for: str | None = None) -> Request:
        headers = []
        if forwarded_for is not None:
            headers.append((b"x-forwarded-for", forwarded_for.encode("ascii")))
        return Request({"type": "http", "client": (ip_address, 12345), "headers": headers})

    def test_terminal_origin_rejection_does_not_consume_ticket(self) -> None:
        ticket = self.app.issue_terminal_ticket(7, "session-digest")

        rejected = self.app.consume_terminal_identity("https://foreign.example", ticket)

        self.assertIsNone(rejected)
        self.assertEqual(
            self.app.consume_terminal_identity("http://127.0.0.1:8080", ticket),
            (7, "session-digest"),
        )

    def test_terminal_ticket_uses_a_credential_free_websocket_url(self) -> None:
        self.assertEqual(
            self.app.terminal_ticket_from_protocol("wargame-terminal, one-use-ticket"),
            "one-use-ticket",
        )
        self.assertIsNone(self.app.terminal_ticket_from_protocol("one-use-ticket"))

    def test_terminal_origin_policy_rejects_missing_malformed_and_non_exact_values(self) -> None:
        rejected = (
            None,
            "null",
            "localhost:8080",
            "ftp://localhost:8080",
            "http://localhost:8080/path",
            "http://localhost:8080?query=1",
            "http://localhost:8080#fragment",
            "http://user@localhost:8080",
            "http://localhost",
            "https://localhost:8080",
        )

        self.assertTrue(all(not self.app.terminal_origin_allowed(value) for value in rejected))
        self.assertTrue(self.app.terminal_origin_allowed("http://localhost:8080"))

    def test_login_limit_survives_restart_state_and_username_rotation(self) -> None:
        ip_address = "198.51.100.24"
        spec = importlib.util.spec_from_file_location("account_app_second_worker", APP_PATH)
        assert spec is not None and spec.loader is not None
        second_worker = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = second_worker
        spec.loader.exec_module(second_worker)
        second_worker.DATA_DIR = self.app.DATA_DIR
        second_worker.DB_PATH = self.app.DB_PATH
        second_worker.initialize_database()
        for index in range(self.app.LOGIN_IP_FAILURE_LIMIT):
            worker = self.app if index % 2 == 0 else second_worker
            worker.record_login_failure("player", f"rotated-{index}", ip_address)

        with self.assertRaises(HTTPException) as limited:
            second_worker.enforce_login_limit("player", "fresh-name", ip_address)

        self.assertEqual(limited.exception.status_code, 429)
        self.assertGreaterEqual(int(limited.exception.headers["Retry-After"]), 1)

    def test_login_limit_prunes_expired_rows(self) -> None:
        with self.app.database() as db:
            db.execute(
                "INSERT INTO login_failures(subject_type,subject,ip_address,failed_at) "
                "VALUES('player','stale','192.0.2.50',0)"
            )

        self.app.enforce_login_limit("player", "stale", "192.0.2.50")

        with self.app.database() as db:
            remaining = db.execute("SELECT COUNT(*) FROM login_failures").fetchone()[0]
        self.assertEqual(remaining, 0)

    def test_login_limit_is_shared_by_subject_and_ip_across_callers(self) -> None:
        for _ in range(self.app.LOGIN_SUBJECT_IP_FAILURE_LIMIT):
            self.app.record_login_failure("admin", "admin", "203.0.113.8")

        with self.assertRaises(HTTPException) as limited:
            self.app.enforce_login_limit("admin", "admin", "203.0.113.8")

        self.assertEqual(limited.exception.status_code, 429)

    def test_success_clears_only_the_matching_subject_ip_bucket(self) -> None:
        for _ in range(self.app.LOGIN_SUBJECT_IP_FAILURE_LIMIT):
            self.app.record_login_failure("player", "pilot", "192.0.2.10")
            self.app.record_login_failure("player", "pilot", "192.0.2.11")

        self.app.clear_login_failures("player", "pilot", "192.0.2.10")

        self.app.enforce_login_limit("player", "pilot", "192.0.2.10")
        with self.assertRaises(HTTPException):
            self.app.enforce_login_limit("player", "pilot", "192.0.2.11")

    def test_forwarded_ip_requires_an_explicit_trusted_proxy(self) -> None:
        forwarded = "198.51.100.90"
        previous = self.app.LOGIN_TRUSTED_PROXY_NETWORKS
        self.addCleanup(setattr, self.app, "LOGIN_TRUSTED_PROXY_NETWORKS", previous)

        self.assertEqual(
            self.app.login_client_ip(self.request("203.0.113.1", forwarded)),
            "203.0.113.1",
        )
        self.app.LOGIN_TRUSTED_PROXY_NETWORKS = self.app.parse_trusted_proxy_networks(
            "203.0.113.0/24"
        )
        self.assertEqual(
            self.app.login_client_ip(self.request("203.0.113.1", forwarded)),
            forwarded,
        )

    def test_origin_configuration_rejects_an_empty_or_wildcard_allowlist(self) -> None:
        for value in ("", "   ", "http://*"):
            with self.assertRaises(RuntimeError):
                self.app.parse_web_shell_allowed_origins(value)

    def test_password_policy_has_eight_character_minimum_and_no_maximum(self) -> None:
        values = ("12345678", "密码密码密码密码", "x" * 4096)

        for index, password in enumerate(values):
            username = f"password-user-{index}"
            created = self.app.create_user(
                self.app.UserBody(
                    username=username,
                    display_name=username,
                    password=password,
                    enabled=True,
                ),
                None,
            )
            with self.app.database() as db:
                row = db.execute(
                    "SELECT password_hash FROM users WHERE id=?", (created["user"]["id"],)
                ).fetchone()
            self.assertNotEqual(row["password_hash"], password)
            self.assertTrue(self.app.verify_password(row["password_hash"], password))
            logged_in = self.app.client_login(
                self.app.LoginBody(username=username, password=password), self.request()
            )
            self.assertTrue(logged_in["token"])
            with self.app.database() as db:
                db.execute(
                    "DELETE FROM sessions WHERE subject_type='player' AND subject_id=?",
                    (created["user"]["id"],),
                )

        for password in ("1", "1234567"):
            with self.assertRaises(ValidationError):
                self.app.UserBody(
                    username="short-password",
                    display_name="Short Password",
                    password=password,
                    enabled=True,
                )

        with self.assertRaises(HTTPException) as missing:
            self.app.create_user(
                self.app.UserBody(
                    username="empty-password",
                    display_name="Empty Password",
                    password="",
                    enabled=True,
                ),
                None,
            )
        self.assertEqual(missing.exception.status_code, 422)

    def test_terminal_password_recheck_uses_login_rate_limit(self) -> None:
        previous = self.app.WEB_SHELL_ENABLED
        self.app.WEB_SHELL_ENABLED = True
        self.addCleanup(setattr, self.app, "WEB_SHELL_ENABLED", previous)
        with self.app.database() as db:
            admin = db.execute("SELECT * FROM admins ORDER BY id LIMIT 1").fetchone()
        request = self.request("198.51.100.77")

        for _ in range(self.app.LOGIN_SUBJECT_IP_FAILURE_LIMIT):
            with self.assertRaises(HTTPException) as rejected:
                self.app.terminal_login(
                    self.app.TerminalLoginBody(password="wrong-password"),
                    request,
                    None,
                    admin,
                )
            self.assertEqual(rejected.exception.status_code, 403)

        with self.assertRaises(HTTPException) as limited:
            self.app.terminal_login(
                self.app.TerminalLoginBody(password="wrong-password"),
                request,
                None,
                admin,
            )
        self.assertEqual(limited.exception.status_code, 429)

    def test_password_models_reject_non_string_json_values(self) -> None:
        with self.assertRaises(ValidationError):
            self.app.LoginBody.model_validate_json('{"username":"admin","password":123}')

    def test_room_directory_requires_internal_auth_and_marks_unhosted_rooms(self) -> None:
        with self.assertRaises(HTTPException) as rejected:
            self.app.list_public_rooms(None)
        self.assertEqual(rejected.exception.status_code, 403)

        self.app.create_room(
            self.app.RoomBody(room_id="secondary", name="Secondary"),
            None,
        )
        listed = self.app.list_public_rooms("i" * 32)
        rooms = {room["roomId"]: room for room in listed["rooms"]}
        self.assertTrue(rooms["main"]["hostedByGameServer"])
        self.assertFalse(rooms["secondary"]["hostedByGameServer"])


if __name__ == "__main__":
    unittest.main()
