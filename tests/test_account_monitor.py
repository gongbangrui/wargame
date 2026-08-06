from __future__ import annotations

import importlib.util
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from typing import TypeAlias

from fastapi import HTTPException
from pydantic import JsonValue

PROJECT_ROOT = Path(__file__).resolve().parents[1]
ACCOUNT_DIR = PROJECT_ROOT / "server" / "account"
APP_PATH = ACCOUNT_DIR / "app.py"
JsonObject: TypeAlias = dict[str, JsonValue]


def conversation_record(
    conversation_id: str,
    timestamp: str,
    status: str = "completed",
) -> JsonObject:
    return {
        "conversationId": conversation_id,
        "requestId": f"request-{conversation_id}",
        "roomId": "main",
        "generation": 4,
        "time": timestamp,
        "model": "qwen3.5:2b",
        "status": status,
        "failure": "" if status == "completed" else status,
        "latencyMs": 42,
        "messages": [
            {"role": "system", "content": "system prompt"},
            {"role": "user", "content": "user prompt"},
        ],
        "raw": {"response": "raw"},
        "parsed": {"commands": []},
        "final": {"engine": "ollama", "commands": []},
        "fallback": None,
    }


class AccountAiConversationMonitorTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        os.environ["ADMIN_PASSWORD"] = "isolated-test-password"
        os.environ["INTERNAL_API_KEY"] = "i" * 32
        sys.path.insert(0, str(ACCOUNT_DIR))
        spec = importlib.util.spec_from_file_location(
            "account_app_ai_monitor", APP_PATH
        )
        assert spec is not None and spec.loader is not None
        cls.app = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = cls.app
        spec.loader.exec_module(cls.app)

    @classmethod
    def tearDownClass(cls) -> None:
        sys.path.remove(str(ACCOUNT_DIR))

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        root = Path(self.temporary.name)
        self.app.DATA_DIR = root
        self.app.DB_PATH = root / "wargame.db"
        self.app.initialize_database()
        with self.app.database() as db:
            admin = db.execute("SELECT * FROM admins ORDER BY id LIMIT 1").fetchone()
            self.admin_token = self.app.create_session(db, "admin", int(admin["id"]))
        self.admin = self.app.require_admin(f"Bearer {self.admin_token}")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_lines(self, filename: str, lines: list[str]) -> None:
        history = self.app.DATA_DIR / "ai-planning-history"
        history.mkdir(exist_ok=True)
        (history / filename).write_text("\n".join(lines) + "\n", encoding="utf-8")

    def test_list_requires_an_authenticated_admin(self) -> None:
        with self.assertRaises(HTTPException) as rejected:
            self.app.require_admin(None)

        self.assertEqual(rejected.exception.status_code, 401)

    def test_list_skips_bad_lines_and_filters_cancelled_records(self) -> None:
        cancelled = conversation_record(
            "conversation-cancelled", "2026-08-06T12:02:00.000Z", "cancelled"
        )
        cancelled["fallback"] = {"engine": "rules", "reason": "request cancelled"}
        completed = conversation_record(
            "conversation-completed", "2026-08-06T12:01:00.000Z"
        )
        self.write_lines(
            "ai-conversations-2026-08-06.jsonl",
            ["{malformed", json.dumps(completed), json.dumps(cancelled)],
        )

        page = self.app.monitor_ai_conversations(
            status="cancelled", limit=500, before=None, _=self.admin
        )
        payload = page.model_dump(by_alias=True, mode="json")

        self.assertEqual(
            [item["conversationId"] for item in payload["conversations"]],
            ["conversation-cancelled"],
        )
        self.assertEqual(payload["limit"], 100)
        self.assertEqual(payload["conversations"][0]["status"], "cancelled")
        self.assertEqual(
            payload["conversations"][0]["rulesFallbackReason"], "request cancelled"
        )

    def test_list_summary_excludes_original_model_content(self) -> None:
        record = conversation_record("conversation-summary", "2026-08-06T12:00:00.000Z")
        record["unknownDiskField"] = "must not be returned"
        self.write_lines("ai-conversations-2026-08-06.jsonl", [json.dumps(record)])

        page = self.app.monitor_ai_conversations(
            status="", limit=10, before=None, _=self.admin
        )
        summary = page.model_dump(by_alias=True, mode="json")["conversations"][0]

        for excluded in (
            "messages",
            "raw",
            "parsed",
            "final",
            "fallback",
            "rawAssistantContent",
            "parsedPlan",
            "executionResult",
            "unknownDiskField",
        ):
            self.assertNotIn(excluded, summary)

    def test_opaque_cursor_pages_without_repeating_records(self) -> None:
        records = [
            conversation_record("conversation-new", "2026-08-06T12:03:00.000Z"),
            conversation_record("conversation-middle", "2026-08-06T12:02:00.000Z"),
            conversation_record("conversation-old", "2026-08-06T12:01:00.000Z"),
        ]
        self.write_lines(
            "ai-conversations-2026-08-06.jsonl",
            [json.dumps(record) for record in records],
        )
        first = self.app.monitor_ai_conversations(
            status="", limit=1, before=None, _=self.admin
        ).model_dump(by_alias=True, mode="json")

        second = self.app.monitor_ai_conversations(
            status="", limit=1, before=first["nextBefore"], _=self.admin
        ).model_dump(by_alias=True, mode="json")

        self.assertNotIn("conversation-new", first["nextBefore"])
        self.assertEqual(
            first["conversations"][0]["conversationId"], "conversation-new"
        )
        self.assertEqual(
            second["conversations"][0]["conversationId"], "conversation-middle"
        )

    def test_invalid_cursor_returns_a_path_free_validation_error(self) -> None:
        with self.assertRaises(HTTPException) as rejected:
            self.app.monitor_ai_conversations(
                status="", limit=10, before="not-a-monitor-cursor", _=self.admin
            )

        self.assertEqual(rejected.exception.status_code, 422)
        self.assertNotIn(str(self.app.DATA_DIR), str(rejected.exception.detail))

    def test_detail_recursively_redacts_keys_and_original_strings(self) -> None:
        record = conversation_record("conversation-secret", "2026-08-06T12:00:00.000Z")
        record["messages"] = [
            {"role": "system", "content": "Authorization: Bearer system-token"},
            {"role": "user", "content": '{"apiKey":"user-key","order":"advance"}'},
        ]
        record["raw"] = {
            "headers": {"Cookie": "sid=cookie-secret"},
            "text": "token=raw-token",
        }
        record["parsed"] = {"privateKey": "parsed-secret", "action": "move"}
        record["final"] = {"credential": "final-secret", "engine": "rules"}
        record["fallback"] = {"reason": "password=fallback-secret", "engine": "rules"}
        record["unknownDiskField"] = "unknown-secret"
        self.write_lines("ai-conversations-2026-08-06.jsonl", [json.dumps(record)])

        detail = self.app.monitor_ai_conversation_detail(
            "conversation-secret", self.admin
        ).model_dump(by_alias=True, mode="json")["conversation"]
        serialized = json.dumps(detail, sort_keys=True)

        for secret in (
            "system-token",
            "user-key",
            "cookie-secret",
            "raw-token",
            "parsed-secret",
            "final-secret",
            "fallback-secret",
            "unknown-secret",
        ):
            self.assertNotIn(secret, serialized)
        self.assertIn("[REDACTED]", serialized)
        self.assertEqual(detail["executionResult"]["engine"], "rules")
        self.assertNotIn("unknownDiskField", detail)

    def test_reader_ignores_unrelated_jsonl_and_symlinked_daily_files(self) -> None:
        leaked = conversation_record("conversation-leaked", "2026-08-06T12:00:00.000Z")
        self.write_lines("unrelated.jsonl", [json.dumps(leaked)])
        outside = self.app.DATA_DIR / "outside.jsonl"
        outside.write_text(json.dumps(leaked) + "\n", encoding="utf-8")
        history = self.app.DATA_DIR / "ai-planning-history"
        (history / "ai-conversations-2026-08-07.jsonl").symlink_to(outside)

        page = self.app.monitor_ai_conversations(
            status="", limit=10, before=None, _=self.admin
        ).model_dump(by_alias=True, mode="json")

        self.assertEqual(page["conversations"], [])

    def test_reader_bounds_aggregate_history_bytes_to_newest_files(self) -> None:
        history = self.app.DATA_DIR / "ai-planning-history"
        history.mkdir()
        padding = "x" * (6 * 1024 * 1024)
        old_record = conversation_record("conversation-old", "2026-08-04T12:00:00.000Z")
        middle_record = conversation_record(
            "conversation-middle", "2026-08-05T12:00:00.000Z"
        )
        new_record = conversation_record("conversation-new", "2026-08-06T12:00:00.000Z")
        (history / "ai-conversations-2026-08-04.jsonl").write_text(
            json.dumps(old_record) + "\n", encoding="utf-8"
        )
        (history / "ai-conversations-2026-08-05.jsonl").write_text(
            padding + "\n" + json.dumps(middle_record) + "\n", encoding="utf-8"
        )
        (history / "ai-conversations-2026-08-06.jsonl").write_text(
            padding + "\n" + json.dumps(new_record) + "\n", encoding="utf-8"
        )

        page = self.app.monitor_ai_conversations(
            status="", limit=10, before=None, _=self.admin
        ).model_dump(by_alias=True, mode="json")

        self.assertEqual(
            [item["conversationId"] for item in page["conversations"]],
            ["conversation-new"],
        )

    def test_detail_rejects_invalid_ids_and_hides_missing_paths(self) -> None:
        with self.assertRaises(HTTPException) as invalid:
            self.app.monitor_ai_conversation_detail("../outside", self.admin)
        with self.assertRaises(HTTPException) as missing:
            self.app.monitor_ai_conversation_detail("conversation-missing", self.admin)

        self.assertEqual(invalid.exception.status_code, 422)
        self.assertEqual(missing.exception.status_code, 404)
        self.assertNotIn(str(self.app.DATA_DIR), str(missing.exception.detail))


if __name__ == "__main__":
    unittest.main()
