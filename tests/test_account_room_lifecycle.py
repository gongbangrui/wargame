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

        with self.assertRaises(HTTPException) as start_rejected:
            self.app.room_action("main", "start", None)
        self.assertEqual(start_rejected.exception.status_code, 409)

        resumed = self.app.room_action("main", "resume", None)

        self.assertEqual(resumed["operation"]["action"], "resume")
        self.assertEqual(resumed["operation"]["expectedStatus"], "running")
        self.assertEqual(resumed["operation"]["state"], "pending")
        self.assertEqual(resumed["room"]["status"], "paused")

        acknowledged = self.app.internal_room_operation_ack(
            "main",
            resumed["operation"]["operationId"],
            self.app.InternalOperationAckBody(
                state="acknowledged",
                revision=resumed["operation"]["requestedRevision"] + 1,
                code="",
            ),
            self.app.INTERNAL_KEY,
        )
        self.assertEqual(acknowledged["room"]["status"], "running")

        with self.app.database() as db:
            db.execute("UPDATE rooms SET status='preparing' WHERE room_id='main'")
        with self.assertRaises(HTTPException) as rejected:
            self.app.room_action("main", "resume", None)
        self.assertEqual(rejected.exception.status_code, 409)

    def test_status_heartbeat_cannot_commit_a_pending_open(self) -> None:
        with self.app.database() as db:
            db.execute(
                "UPDATE rooms SET status='finished',winner='blue',status_reason='completed' "
                "WHERE room_id='main'"
            )

        requested = self.app.room_action("main", "open", None)
        heartbeat = self.app.internal_room_status(
            "main",
            self.app.InternalRoomStatusBody(
                status="preparing", reason="game process is preparing"
            ),
            self.app.INTERNAL_KEY,
        )

        self.assertEqual(heartbeat["reason"], "pending operation")
        self.assertEqual(heartbeat["room"]["status"], "finished")
        self.assertEqual(heartbeat["room"]["winner"], "blue")
        self.assertEqual(heartbeat["room"]["operation"]["state"], "pending")

        failed = self.app.internal_room_operation_ack(
            "main",
            requested["operation"]["operationId"],
            self.app.InternalOperationAckBody(
                state="failed",
                revision=requested["operation"]["requestedRevision"],
                code="AI_PROBE_FAILED",
            ),
            self.app.INTERNAL_KEY,
        )
        self.assertEqual(failed["room"]["status"], "stopped")
        self.assertEqual(failed["room"]["winner"], "")
        self.assertEqual(failed["room"]["statusReason"], "AI_PROBE_FAILED")

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

    def test_internal_room_config_persists_strict_vmf_profile(self) -> None:
        with self.app.database() as db:
            db.execute("UPDATE rooms SET status='preparing' WHERE room_id='main'")
        response = self.app.internal_room_config(
            "main",
            self.app.InternalRoomConfigBody(
                expected_config_version=1,
                name="Strict VMF Room",
                description="",
                scenario_id="default",
                protocol_profile="vmf-guided-strike-v1",
            ),
            self.app.INTERNAL_KEY,
        )

        self.assertEqual(response["room"]["protocolProfile"], "vmf-guided-strike-v1")
        self.assertEqual(response["room"]["configVersion"], 2)
        with self.app.database() as db:
            stored = db.execute(
                "SELECT protocol_profile FROM rooms WHERE room_id='main'"
            ).fetchone()[0]
        self.assertEqual(stored, "vmf-guided-strike-v1")

    def test_internal_room_config_persists_demo_v2_profile(self) -> None:
        with self.app.database() as db:
            db.execute("UPDATE rooms SET status='preparing' WHERE room_id='main'")
        response = self.app.internal_room_config(
            "main",
            self.app.InternalRoomConfigBody(
                expected_config_version=1,
                name="VMF Demo Room",
                description="six-step demonstration",
                scenario_id="default",
                protocol_profile="vmf-demo-v2",
            ),
            self.app.INTERNAL_KEY,
        )

        room = response["room"]
        self.assertEqual(room["protocolProfile"], "vmf-demo-v2")
        self.assertEqual(room["operationMode"], "vmf-single-side")
        self.assertEqual(room["participantSide"], "red")
        self.assertEqual(room["fixedTargetSide"], "blue")

    def test_single_side_vmf_profiles_reject_pve(self) -> None:
        for profile in ("vmf-guided-strike-v1", "vmf-demo-v2"):
            with self.subTest(profile=profile), self.assertRaises(ValueError):
                self.app.RoomBody(
                    room_id="vmf-pve",
                    name="Invalid VMF room",
                    protocol_profile=profile,
                    mode="pve",
                )

        with self.app.database() as db:
            db.execute(
                "UPDATE rooms SET status='stopped',mode='pve' WHERE room_id='main'"
            )
        body = self.app.RoomBody(
            room_id="main",
            name="Main Room",
            protocol_profile="vmf-guided-strike-v1",
        )
        with self.assertRaises(HTTPException) as rejected:
            self.app.update_room("main", body, None)
        self.assertEqual(rejected.exception.status_code, 422)

    def test_stopped_legacy_strict_pve_room_migrates_to_pvp(self) -> None:
        with self.app.database() as db:
            db.execute(
                "UPDATE rooms SET protocol_profile='vmf-guided-strike-v1',"
                "mode='pve',ai_provider='ollama',ai_model='legacy-model',"
                "ai_resolved_model='resolved',status='stopped',config_version=7 "
                "WHERE room_id='main'"
            )

        self.app.initialize_database()

        with self.app.database() as db:
            room = db.execute(
                "SELECT mode,ai_provider,ai_model,ai_resolved_model,config_version "
                "FROM rooms WHERE room_id='main'"
            ).fetchone()
        self.assertEqual(room["mode"], "pvp")
        self.assertEqual(room["ai_provider"], "rules")
        self.assertEqual(room["ai_model"], "")
        self.assertEqual(room["ai_resolved_model"], "")
        self.assertEqual(room["config_version"], 8)

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

    def test_single_character_user_credentials_are_valid_but_empty_values_are_rejected(self) -> None:
        created = self.app.create_user(
            self.app.UserBody(
                username="x",
                display_name="x",
                password="p",
                enabled=True,
            ),
            None,
        )
        self.assertEqual(created["user"]["username"], "x")
        with self.assertRaises(ValueError):
            self.app.UserBody(username=" ", display_name="x", password="p")
        with self.assertRaises(ValueError):
            self.app.LoginBody(username="x", password="")

    def test_long_credentials_and_password_whitespace_are_preserved(self) -> None:
        username = "user-" + "x" * 256
        password = " p " + "y" * 256
        created = self.app.create_user(
            self.app.UserBody(
                username=username,
                display_name="long credentials",
                password=password,
                enabled=True,
            ),
            None,
        )
        self.assertEqual(created["user"]["username"], username)
        with self.app.database() as db:
            row = db.execute(
                "SELECT password_hash FROM users WHERE username = ?",
                (username,),
            ).fetchone()
        self.assertIsNotNone(row)
        self.assertTrue(self.app.verify_password(row["password_hash"], password))

if __name__ == "__main__":
    unittest.main()
