"""Local CLI checks; uses only the Python standard library and synthetic task data."""

import json
from contextlib import closing
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest


EXECUTABLE = Path(sys.argv.pop(1)).resolve()


class LocalCliWorkflow(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="rifelove-cli-")
        self.addCleanup(self.directory.cleanup)
        self.database = Path(self.directory.name) / "queue.sqlite"

    def run_cli(self, *arguments, expected_code=0):
        result = subprocess.run(
            [str(EXECUTABLE), *(str(argument) for argument in arguments)],
            capture_output=True, encoding="utf-8", timeout=10,
        )
        self.assertEqual(result.returncode, expected_code, result.stderr)
        return result

    def create_running_task(self):
        created = json.loads(self.run_cli("task-create", self.database, "input.mp4", "output.mp4").stdout)
        with closing(sqlite3.connect(self.database)) as database:
            database.execute("UPDATE tasks SET state='running' WHERE id=?", (created["id"],))
            database.commit()
        return created["id"]

    def test_listing_preserves_running_state(self):
        task_id = self.create_running_task()
        task = json.loads(self.run_cli("tasks", self.database).stdout)
        self.assertEqual(task["id"], task_id)
        self.assertEqual(task["state"], "running")
        with closing(sqlite3.connect(self.database)) as database:
            self.assertEqual(database.execute("SELECT state FROM tasks WHERE id=?", (task_id,)).fetchone()[0], "running")

    def test_recovery_is_explicit_and_idempotent(self):
        self.create_running_task()
        self.assertEqual(json.loads(self.run_cli("tasks-recover", self.database).stdout), {"recovered": 1})
        task = json.loads(self.run_cli("tasks", self.database).stdout)
        self.assertEqual(task["state"], "interrupted")
        self.assertIn("not yet implemented", task["error"])
        self.assertEqual(json.loads(self.run_cli("tasks-recover", self.database).stdout), {"recovered": 0})

    def test_unicode_paths_and_json_escaping_round_trip(self):
        parent = Path(self.directory.name) / "中文 队列"
        parent.mkdir()
        database = parent / "任务.sqlite"
        source = 'C:\\录屏\\测试 "A".mp4'
        output = 'C:\\导出\\结果.mp4'
        self.run_cli("task-create", database, source, output)
        self.assertTrue(database.is_file())
        task = json.loads(self.run_cli("tasks", database).stdout)
        self.assertEqual(task["source"], source)
        self.assertEqual(task["output"], output)

    def test_queries_and_recovery_do_not_create_missing_database(self):
        self.run_cli("tasks", self.database, expected_code=2)
        self.run_cli("tasks-recover", self.database, expected_code=2)
        self.assertFalse(self.database.exists())

    def test_exact_timeline_json(self):
        result = self.run_cli("timeline", "--fps", "60", "--end", "1/6", "--pts", "0,1/24,1/12,1/8")
        frames = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(len(frames), 10)
        self.assertEqual(frames[1]["pts"], "1/60")
        self.assertEqual(frames[1]["timestep"], "2/5")
        self.assertTrue(frames[-1]["terminal_hold"])

    def test_invalid_timeline_fails_without_partial_json(self):
        result = self.run_cli("timeline", "--fps", "0", "--end", "1", "--pts", "0", expected_code=2)
        self.assertEqual(result.stdout, "")
        self.assertIn("positive", result.stderr)


if __name__ == "__main__":
    unittest.main()
