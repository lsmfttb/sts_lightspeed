from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest


ISSUE24_DIR = Path(__file__).resolve().parents[1] / "issue24"
sys.path.insert(0, str(ISSUE24_DIR))

import run_natural_search as study  # noqa: E402


class TimeoutSupervisorTests(unittest.TestCase):
    def test_kills_hung_seed_and_preserves_last_atomic_checkpoint(self) -> None:
        with tempfile.TemporaryDirectory(prefix="issue25-supervisor-") as temp_dir:
            progress_path = Path(temp_dir) / "progress.json"
            child_code = r'''
import json
import os
from pathlib import Path
import sys
import time

path = Path(sys.argv[1])
def checkpoint(boundary, details):
    payload = {
        "schema_id": "issue25-run-progress-v1",
        "status": "running",
        "last_completed_boundary": boundary,
        "details": details,
    }
    temp = path.with_suffix(path.suffix + ".tmp")
    temp.write_text(json.dumps(payload), encoding="utf-8")
    os.replace(temp, path)

checkpoint("seed_run_starting", {"seed": 49})
time.sleep(0.1)
checkpoint("controlled_action_executed", {"seed": 49, "step_index": 3, "screen": "BATTLE"})
time.sleep(30)
'''
            result = study._supervise_process(
                [sys.executable, "-c", child_code, str(progress_path)],
                progress_path=progress_path,
                max_wall_seconds=0.5,
            )

            self.assertTrue(result["timed_out"])
            self.assertEqual(result["seed"], 49)
            self.assertLess(result["elapsed_seconds"], 2.0)
            self.assertNotEqual(result["returncode"], 0)
            progress = json.loads(progress_path.read_text(encoding="utf-8"))
            self.assertEqual(progress["status"], "timed_out")
            self.assertEqual(progress["last_completed_boundary"], "controlled_action_executed")
            self.assertEqual(progress["details"]["step_index"], 3)
            self.assertEqual(
                progress["supervisor_timeout"]["stop_classification"],
                "WALL_BUDGET",
            )
            self.assertTrue(progress["supervisor_timeout"]["terminated_process_tree"])


if __name__ == "__main__":
    unittest.main()
