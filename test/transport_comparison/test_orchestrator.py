#!/usr/bin/env python3
"""验证中断续跑的证据隔离；替身进程只失败退出，不运行实际压力负载。"""
import importlib.util
import json
import pathlib
import tempfile
import types
import unittest

HERE = pathlib.Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("comparison_runner", HERE/"run.py")
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)

class EvidenceIsolation(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="comparison-guard-", dir="/var/tmp")
        self.work = pathlib.Path(self.tmp.name)
        (self.work/"build").mkdir()
        (self.work/"cases").mkdir()
        self.binary = self.work/"build/comparison"
        self.binary.write_text("#!/bin/sh\nexit 2\n")
        self.binary.chmod(0o755)
        self.args = types.SimpleNamespace(suite="guard", work=self.work, rerun=False, workers=32,
                                         publishers=4, domain=173)
        self.case_id = "guard-pubsub-shm-8-n1-r1"

    def tearDown(self):
        self.tmp.cleanup()

    def call(self):
        return runner.run_case(self.args, "shm", "pubsub", 8, 1, 1, .01)

    def test_failed_rerun_cannot_reuse_previous_metrics(self):
        directory = self.work/"runs"/self.case_id
        directory.mkdir(parents=True)
        for role in ("pub","sub"):
            (directory/f"{role}.json").write_text(json.dumps({"sent":999999,"received":999999}))
        result = self.call()
        self.assertEqual(result["status"], "failed")
        self.assertNotIn("pub", result)
        self.assertNotIn("sub", result)
        self.assertFalse((directory/"pub.json").exists())
        self.assertFalse((directory/"sub.json").exists())

    def test_resume_rejects_changed_executable(self):
        self.call()
        self.binary.write_text("#!/bin/sh\nexit 3\n")
        with self.assertRaisesRegex(RuntimeError, "二进制"):
            self.call()

if __name__=="__main__":
    unittest.main()
