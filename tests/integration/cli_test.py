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
        self.run_cli("queue-report", self.database, expected_code=2)
        self.assertFalse(self.database.exists())

    def test_queue_report_is_read_only_and_summarizes_completed_and_failed_tasks(self):
        completed = json.loads(self.run_cli("task-create", self.database, "完成源.mp4", "完成结果.mkv").stdout)
        failed = json.loads(self.run_cli("task-create", self.database, "失败源.mp4", "失败结果.mkv").stdout)
        result = {
            "outputFrames": 120, "aiFrames": 60, "elapsedSeconds": 60.0,
            "processingFps": 2.0, "sceneCuts": 1, "duplicatePairs": 2, "abnormalPairs": 3,
        }
        with closing(sqlite3.connect(self.database)) as database:
            database.execute("UPDATE tasks SET state='completed' WHERE id=?", (completed["id"],))
            database.execute("UPDATE tasks SET state='failed',error=? WHERE id=?", ("test failure", failed["id"]))
            database.execute(
                "INSERT INTO task_details(task_id,configuration,identity,progress,result) VALUES(?,?,?,?,?)",
                (completed["id"], json.dumps({"targetRate": "240"}), "", "", json.dumps(result)),
            )
            database.commit()

        report_path = Path(self.directory.name) / "queue-report.json"
        self.run_cli("queue-report", self.database, "--output", report_path)
        report = json.loads(report_path.read_text(encoding="utf-8"))
        self.assertEqual(report["schemaVersion"], 1)
        self.assertEqual(report["kind"], "rifelove_queue_report")
        self.assertEqual(report["summary"]["taskCount"], 2)
        self.assertEqual(report["summary"]["states"]["completed"], 1)
        self.assertEqual(report["summary"]["states"]["failed"], 1)
        self.assertEqual(report["summary"]["completedOutputFrames"], 120)
        self.assertEqual(report["summary"]["completedAiFrames"], 60)
        self.assertEqual(report["summary"]["aggregateCompletedOutputFps"], 2.0)
        self.assertEqual(report["summary"]["sceneCuts"], 1)
        self.assertFalse(report["qualityDiagnostics"]["perceptualMetricsMeasured"])
        self.assertEqual(report["failures"], [{"id": failed["id"], "state": "failed", "error": "test failure"}])
        self.assertEqual(len(report["tasks"]), 2)
        self.assertEqual(report["tasks"][0]["source"], "完成源.mp4")
        with closing(sqlite3.connect(self.database)) as database:
            states = dict(database.execute("SELECT id,state FROM tasks"))
        self.assertEqual(states[completed["id"]], "completed")
        self.assertEqual(states[failed["id"]], "failed")

        report_path.write_text("keep this file", encoding="utf-8")
        self.run_cli("queue-report", self.database, "--output", report_path, expected_code=2)
        self.assertEqual(report_path.read_text(encoding="utf-8"), "keep this file")

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
