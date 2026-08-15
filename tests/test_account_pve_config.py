from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

from fastapi import HTTPException


APP_PATH = Path(__file__).resolve().parents[1] / "server" / "account" / "app.py"


class AccountPveConfigTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        os.environ["ADMIN_PASSWORD"] = "isolated-test-password"
        os.environ["INTERNAL_API_KEY"] = "i" * 32
        os.environ["AI_PROVIDER"] = "auto"
        os.environ["OLLAMA_BASE_URL"] = "http://host.docker.internal:11434"
        os.environ["OLLAMA_MODEL"] = "auto"
        spec = importlib.util.spec_from_file_location("account_app_pve_contract", APP_PATH)
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
        self.admin = self._admin()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def _admin(self):
        with self.app.database() as db:
            return db.execute("SELECT * FROM admins ORDER BY id LIMIT 1").fetchone()

    def _room_body(self, **overrides):
        values = {
            "room_id": "pve-room",
            "name": "PVE Room",
            "description": "AI room",
            "scenario_id": "default",
            "seat_limits": {},
            "seat_parameters": {},
            "enabled": True,
        }
        values.update(overrides)
        return self.app.RoomBody(**values)

    def test_new_database_defaults_to_pvp_normal_version_one(self) -> None:
        with self.app.database() as db:
            row = db.execute(
                "SELECT mode, ai_difficulty, config_version FROM rooms WHERE room_id='main'"
            ).fetchone()

        self.assertEqual(row["mode"], "pvp")
        self.assertEqual(row["ai_difficulty"], "normal")
        self.assertEqual(row["config_version"], 1)

    def test_create_and_update_pve_increments_version(self) -> None:
        created = self.app.create_room(
            self._room_body(mode="pve", ai_difficulty="hard"), self.admin
        )
        self.assertEqual(created["room"]["mode"], "pve")
        self.assertEqual(created["room"]["aiDifficulty"], "hard")
        self.assertEqual(created["room"]["configVersion"], 1)

        updated = self.app.update_room(
            "pve-room",
            self._room_body(mode="pve", ai_difficulty="easy"),
            self.admin,
        )
        self.assertEqual(updated["room"]["aiDifficulty"], "easy")
        self.assertEqual(updated["room"]["configVersion"], 2)

    def test_legacy_put_without_new_fields_preserves_pve_configuration(self) -> None:
        self.app.create_room(
            self._room_body(mode="pve", ai_difficulty="hard"), self.admin
        )
        legacy = self._room_body()
        updated = self.app.update_room("pve-room", legacy, self.admin)

        self.assertEqual(updated["room"]["mode"], "pve")
        self.assertEqual(updated["room"]["aiDifficulty"], "hard")
        self.assertEqual(updated["room"]["configVersion"], 1)

    def test_legacy_put_without_intel_fields_preserves_custom_windows(self) -> None:
        self.app.create_room(
            self._room_body(intel_stale_after_sec=25.0, intel_archive_after_sec=300.0),
            self.admin,
        )
        updated = self.app.update_room("pve-room", self._room_body(name="Renamed"), self.admin)

        self.assertEqual(updated["room"]["intelStaleAfterSec"], 25.0)
        self.assertEqual(updated["room"]["intelArchiveAfterSec"], 300.0)

    def test_running_room_rejects_mode_or_difficulty_change(self) -> None:
        self.app.create_room(
            self._room_body(mode="pve", ai_difficulty="normal"), self.admin
        )
        with self.app.database() as db:
            db.execute("UPDATE rooms SET status='running' WHERE room_id='pve-room'")

        with self.assertRaises(HTTPException) as rejected:
            self.app.update_room(
                "pve-room",
                self._room_body(mode="pvp", ai_difficulty="hard"),
                self.admin,
            )

        self.assertEqual(rejected.exception.status_code, 409)

    def test_invalid_mode_and_difficulty_are_rejected_at_boundary(self) -> None:
        with self.assertRaises(ValueError):
            self._room_body(mode="coop")
        with self.assertRaises(ValueError):
            self._room_body(ai_difficulty="extreme")

    def test_ai_config_defaults_to_auto_and_internal_rooms_exposes_it(self) -> None:
        with self.app.database() as db:
            row = db.execute("SELECT * FROM ai_config WHERE config_id=1").fetchone()

        self.assertEqual(row["provider"], "auto")
        self.assertEqual(row["base_url"], "http://host.docker.internal:11434")
        self.assertEqual(row["model"], "auto")
        internal = self.app.internal_rooms("i" * 32)
        self.assertEqual(internal["aiConfig"]["model"], "auto")
        self.assertEqual(internal["aiConfig"]["ollamaPort"], 11434)

    def test_missing_environment_model_defaults_to_auto(self) -> None:
        with patch.dict(os.environ, {}, clear=True):
            config = self.app.environment_ai_config()

        self.assertEqual(config["baseUrl"], "http://host.docker.internal:11434")
        self.assertEqual(config["model"], "auto")

    def test_untouched_legacy_default_migrates_once_to_auto(self) -> None:
        with self.app.database() as db:
            db.execute(
                "UPDATE ai_config SET provider='auto', model='qwen3.5:4b', "
                "config_version=1 WHERE config_id=1"
            )

        self.app.initialize_database()

        with self.app.database() as db:
            migrated = db.execute(
                "SELECT provider, base_url, model, config_version FROM ai_config "
                "WHERE config_id=1"
            ).fetchone()
        self.assertEqual(migrated["provider"], "auto")
        self.assertEqual(migrated["base_url"], "http://host.docker.internal:11434")
        self.assertEqual(migrated["model"], "auto")
        self.assertEqual(migrated["config_version"], 2)

        self.app.initialize_database()

        with self.app.database() as db:
            repeated = db.execute("SELECT * FROM ai_config WHERE config_id=1").fetchone()
        self.assertEqual(repeated["model"], "auto")
        self.assertEqual(repeated["config_version"], 2)

    def test_hand_modified_provider_does_not_migrate_legacy_model(self) -> None:
        with self.app.database() as db:
            db.execute(
                "UPDATE ai_config SET provider='ollama', model='qwen3.5:4b', "
                "config_version=1 WHERE config_id=1"
            )

        self.app.initialize_database()

        with self.app.database() as db:
            row = db.execute("SELECT provider, model, config_version FROM ai_config").fetchone()
        self.assertEqual(dict(row), {
            "provider": "ollama",
            "model": "qwen3.5:4b",
            "config_version": 1,
        })

    def test_hand_modified_base_url_does_not_migrate_legacy_model(self) -> None:
        with self.app.database() as db:
            db.execute(
                "UPDATE ai_config SET base_url='http://127.0.0.1:11434', "
                "provider='auto', model='qwen3.5:4b', config_version=1 "
                "WHERE config_id=1"
            )

        self.app.initialize_database()

        with self.app.database() as db:
            row = db.execute(
                "SELECT provider, base_url, model, config_version FROM ai_config"
            ).fetchone()
        self.assertEqual(dict(row), {
            "provider": "auto",
            "base_url": "http://127.0.0.1:11434",
            "model": "qwen3.5:4b",
            "config_version": 1,
        })

    def test_hand_modified_model_does_not_migrate_legacy_provider(self) -> None:
        with self.app.database() as db:
            db.execute(
                "UPDATE ai_config SET provider='auto', model='explicit-model', "
                "config_version=1 WHERE config_id=1"
            )

        self.app.initialize_database()

        with self.app.database() as db:
            row = db.execute("SELECT provider, model, config_version FROM ai_config").fetchone()
        self.assertEqual(dict(row), {
            "provider": "auto",
            "model": "explicit-model",
            "config_version": 1,
        })

    def test_hand_modified_version_does_not_migrate_legacy_tuple(self) -> None:
        with self.app.database() as db:
            db.execute(
                "UPDATE ai_config SET provider='auto', model='qwen3.5:4b', "
                "config_version=2 WHERE config_id=1"
            )

        self.app.initialize_database()

        with self.app.database() as db:
            row = db.execute("SELECT provider, model, config_version FROM ai_config").fetchone()
        self.assertEqual(dict(row), {
            "provider": "auto",
            "model": "qwen3.5:4b",
            "config_version": 2,
        })

    def test_explicit_environment_model_is_persisted_without_normalization(self) -> None:
        with self.app.database() as db:
            db.execute("DELETE FROM ai_config WHERE config_id=1")

        with patch.dict(os.environ, {"OLLAMA_MODEL": "explicit-model"}, clear=False):
            self.app.initialize_database()

        with self.app.database() as db:
            row = db.execute("SELECT model, config_version FROM ai_config").fetchone()
        self.assertEqual(row["model"], "explicit-model")
        self.assertEqual(row["config_version"], 1)

    def test_admin_ai_config_update_increments_version_only_when_changed(self) -> None:
        initial = self.app.admin_ai_config(self.admin)["aiConfig"]
        updated = self.app.update_admin_ai_config(
            self.app.AiConfigBody(
                provider="ollama",
                ollama_scheme="http",
                ollama_host="127.0.0.1",
                ollama_port=11434,
                model="explicit-model",
            ),
            self.admin,
        )["aiConfig"]
        unchanged = self.app.update_admin_ai_config(
            self.app.AiConfigBody(
                provider="ollama",
                ollama_scheme="http",
                ollama_host="127.0.0.1",
                ollama_port=11434,
                model="explicit-model",
            ),
            self.admin,
        )["aiConfig"]

        self.assertEqual(initial["configVersion"], 1)
        self.assertEqual(updated["configVersion"], 2)
        self.assertEqual(updated["baseUrl"], "http://127.0.0.1:11434")
        self.assertEqual(unchanged["configVersion"], 2)

    def test_ai_config_rejects_invalid_boundary_values(self) -> None:
        with self.assertRaises(ValueError):
            self.app.AiConfigBody(
                provider="ollama",
                ollama_scheme="http",
                ollama_host="127.0.0.1",
                ollama_port=65536,
                model="explicit-model",
            )
        with self.assertRaises(ValueError):
            self.app.AiConfigBody(
                provider="ollama",
                ollama_scheme="http",
                ollama_host="http://127.0.0.1",
                ollama_port=11434,
                model="explicit-model",
            )

    def test_ai_status_preserves_response_too_large_without_leaking_extra_fields(self) -> None:
        status = self.app.redacted_ai_status(
            {
                "roomState": {"roomMode": "pve"},
                "ai": {
                    "provider": "ollama",
                    "connectionStatus": "response_too_large",
                    "probeFailureClass": "response_too_large",
                    "lastFailureClass": "response_too_large",
                    "model": "explicit-model",
                    "baseUrl": "http://127.0.0.1:11434",
                    "requests": 2,
                },
            }
        )

        self.assertTrue(status["active"])
        self.assertEqual(status["connectionStatus"], "response_too_large")
        self.assertEqual(status["probeFailureClass"], "response_too_large")
        self.assertEqual(status["lastFailureClass"], "response_too_large")
        self.assertNotIn("projection", status)


if __name__ == "__main__":
    unittest.main()
