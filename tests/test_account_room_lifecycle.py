from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
import unittest

from fastapi import HTTPException


APP_PATH = Path(__file__).resolve().parents[1] / "server" / "account" / "app.py"


class AccountRoomLifecycleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        os.environ["ADMIN_PASSWORD"] = "isolated-test-password"
        os.environ["INTERNAL_API_KEY"] = "i" * 32
        spec = importlib.util.spec_from_file_location("account_app_contract", APP_PATH)
        assert spec is not None and spec.loader is not None
        cls.app = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = cls.app
        spec.loader.exec_module(cls.app)

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        root = Path(self.temporary.name)
        self.app.DATA_DIR = root
        self.app.DB_PATH = root / "wargame.db"
        self.app.GAME_STATUS_PATH = root / "game-status.json"
        self.app.INTERNAL_KEY = "i" * 32
        self.app.initialize_database()
        self.app.GAME_STATUS_PATH.write_text(
            json.dumps({"roomState": {"roomId": "main", "lifecycleRevision": 17}}),
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def create_active_player(self, username: str = "pilot") -> tuple[int, str]:
        created = self.app.create_user(
            self.app.UserBody(
                username=username,
                display_name="Pilot",
                password="test-password",
                enabled=True,
            ),
            None,
        )
        user_id = created["user"]["id"]
        with self.app.database() as db:
            token = self.app.create_session(db, "player", user_id)
        return user_id, token

    def pending_invalidation_count(self, user_id: int) -> int:
        with self.app.database() as db:
            return db.execute(
                "SELECT COUNT(*) FROM user_kick_requests "
                "WHERE user_id=? AND processed_at IS NULL",
                (user_id,),
            ).fetchone()[0]

    def test_reset_waits_for_revisioned_idempotent_ack(self) -> None:
        with self.app.database() as db:
            db.execute("UPDATE rooms SET status='running' WHERE room_id='main'")

        requested = self.app.room_action("main", "reset", None)
        operation = requested["operation"]
        self.assertEqual(operation["requestedRevision"], 17)
        self.assertEqual(operation["state"], "pending")
        self.assertEqual(requested["room"]["status"], "running")

        body = self.app.InternalOperationAckBody(
            state="acknowledged", revision=18, code=""
        )
        acknowledged = self.app.internal_room_operation_ack(
            "main", operation["operationId"], body, self.app.INTERNAL_KEY
        )
        self.assertEqual(acknowledged["room"]["status"], "preparing")
        self.assertEqual(acknowledged["room"]["operation"]["appliedRevision"], 18)

        duplicate = self.app.internal_room_operation_ack(
            "main", operation["operationId"], body, self.app.INTERNAL_KEY
        )
        self.assertTrue(duplicate["alreadyProcessed"])
        with self.app.database() as db:
            db.execute("UPDATE rooms SET status='running' WHERE room_id='main'")
        second = self.app.room_action("main", "redeploy", None)["operation"]
        with self.assertRaises(HTTPException) as stale_revision:
            self.app.internal_room_operation_ack(
                "main",
                second["operationId"],
                self.app.InternalOperationAckBody(
                    state="acknowledged", revision=17, code=""
                ),
                self.app.INTERNAL_KEY,
            )
        self.assertEqual(stale_revision.exception.status_code, 409)
        with self.assertRaises(HTTPException) as conflict:
            self.app.internal_room_operation_ack(
                "main",
                operation["operationId"],
                self.app.InternalOperationAckBody(
                    state="failed", revision=18, code="STALE_REVISION"
                ),
                self.app.INTERNAL_KEY,
            )
        self.assertEqual(conflict.exception.status_code, 409)

    def test_redeploy_ack_accepts_revision_advanced_past_monitor_snapshot(self) -> None:
        with self.app.database() as db:
            db.execute("UPDATE rooms SET status='running' WHERE room_id='main'")

        operation = self.app.room_action("main", "redeploy", None)["operation"]
        acknowledged = self.app.internal_room_operation_ack(
            "main",
            operation["operationId"],
            self.app.InternalOperationAckBody(
                state="acknowledged", revision=operation["requestedRevision"] + 3, code=""
            ),
            self.app.INTERNAL_KEY,
        )

        self.assertEqual(acknowledged["room"]["status"], "preparing")
        self.assertEqual(
            acknowledged["room"]["operation"]["appliedRevision"],
            operation["requestedRevision"] + 3,
        )

    def test_resume_is_available_only_for_a_paused_room_without_rechecking_readiness(self) -> None:
        with self.app.database() as db:
            db.execute("UPDATE rooms SET status='paused' WHERE room_id='main'")

        resumed = self.app.room_action("main", "resume", None)

        self.assertEqual(resumed["operation"]["action"], "resume")
        self.assertEqual(resumed["operation"]["expectedStatus"], "running")
        self.assertEqual(resumed["room"]["status"], "running")

        with self.app.database() as db:
            db.execute("UPDATE rooms SET status='preparing' WHERE room_id='main'")
        with self.assertRaises(HTTPException) as rejected:
            self.app.room_action("main", "resume", None)
        self.assertEqual(rejected.exception.status_code, 409)

    def test_finished_status_writes_back_winner_and_reason(self) -> None:
        response = self.app.internal_room_status(
            "main",
            self.app.InternalRoomStatusBody(
                status="finished", reason="commander disconnected", winner="blue"
            ),
            self.app.INTERNAL_KEY,
        )
        self.assertEqual(response["room"]["status"], "finished")
        self.assertEqual(response["room"]["winner"], "blue")
        self.assertEqual(response["room"]["statusReason"], "commander disconnected")

    def test_concurrent_lifecycle_operations_leave_one_pending(self) -> None:
        self.app.GAME_STATUS_PATH.write_text(
            json.dumps({"roomState": {"roomId": "main", "lifecycleRevision": 17}}),
            encoding="utf-8",
        )

        def request_reset() -> int:
            try:
                self.app.room_action("main", "reset", None)
            except HTTPException as error:
                return error.status_code
            return 200

        with ThreadPoolExecutor(max_workers=2) as pool:
            statuses = list(pool.map(lambda _: request_reset(), range(2)))
        self.assertEqual(sorted(statuses), [200, 409])

    def test_client_logout_emits_socket_invalidation_request(self) -> None:
        user_id, token = self.create_active_player()

        self.app.client_logout(f"Bearer {token}")

        self.assertEqual(self.pending_invalidation_count(user_id), 1)
        with self.app.database() as db:
            remaining_sessions = db.execute(
                "SELECT COUNT(*) FROM sessions "
                "WHERE subject_type='player' AND subject_id=?",
                (user_id,),
            ).fetchone()[0]
        self.assertEqual(remaining_sessions, 0)

    def test_user_update_emits_socket_invalidation_request(self) -> None:
        user_id, _ = self.create_active_player()
        with self.app.database() as db:
            admin = db.execute("SELECT * FROM admins ORDER BY id LIMIT 1").fetchone()

        self.app.update_user(
            user_id,
            self.app.UserBody(
                username="pilot",
                display_name="Updated Pilot",
                password="",
                enabled=True,
            ),
            admin,
        )

        self.assertEqual(self.pending_invalidation_count(user_id), 1)

    def test_user_delete_emits_socket_invalidation_request(self) -> None:
        user_id, _ = self.create_active_player()
        with self.app.database() as db:
            admin = db.execute("SELECT * FROM admins ORDER BY id LIMIT 1").fetchone()

        self.app.delete_user(user_id, admin)

        self.assertEqual(self.pending_invalidation_count(user_id), 1)

if __name__ == "__main__":
    unittest.main()
