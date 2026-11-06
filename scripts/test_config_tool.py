#!/usr/bin/env python3
"""配置管理工具的隔离故障测试；不启动 Docker、不连接实际部署。"""
from contextlib import redirect_stdout
import copy
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import combat_config as tool


def config(version, damage=18):
    # 此处测试发布协议而非 C++ schema；最终数值约束由两个 validate 接口负责。
    return {"version": version, "skills": {"strike": {"damage": damage}}}


class FakeRPC:
    def __init__(self, directory, initial):
        self.directory = directory
        self.nodes = {service: {"config": copy.deepcopy(initial), "version": initial["version"],
                                "generation": 1, "applied_tick": 0} for service in tool.SERVICES}
        self.calls = []
        self.before = None
        self.after = None
        self.normalize = {}

    def __call__(self, service, operation, payload):
        self.calls.append((service, operation, copy.deepcopy(payload)))
        if self.before:
            self.before(service, operation, payload)
        node = self.nodes[service]
        if operation == "status":
            result = copy.deepcopy(node)
        else:
            if payload["expected_generation"] != node["generation"]:
                raise tool.ToolError("HTTP 409 generation conflict")
            if operation == "validate":
                candidate = self.normalize.get(service, payload["config"])
                result = {"valid": True, "config": copy.deepcopy(candidate), "version": candidate["version"],
                          "current_version": node["version"], "generation": node["generation"],
                          "applied_tick": node["applied_tick"], "would_change": candidate != node["config"]}
            else:
                candidate, _ = tool.read_config(self.directory / "current.json")
                changed = candidate != node["config"]
                if changed:
                    node.update(config=candidate, version=candidate["version"],
                                generation=node["generation"] + 1, applied_tick=100 + node["generation"])
                result = dict(copy.deepcopy(node), changed=changed)
        if self.after:
            self.after(service, operation, payload, result)
        return result


class ConfigToolTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.deploy = self.root / "deploy"
        self.deploy.mkdir()
        self.directory = self.deploy / "combat-config"
        self.env = self.deploy / "scene-host.env"
        self.compose = self.deploy / "scene-host.compose.yaml"
        self.compose.write_text("services: {}\n", encoding="utf-8")
        self.default = self.root / "default.json"
        self.old, self.new = config("test-v1"), config("test-v2", 25)
        self.default.write_bytes(tool.encode_json(self.old))
        self.env.write_bytes(b'INTERNAL_TOKEN="local-test-secret$keep"\r\nOTHER=value\r\nCOMBAT_CONFIG_FILE=\r\n')
        self.paths = tool.Paths(self.env, self.compose, self.directory, "test-project")
        self.default_patch = patch.object(tool, "DEFAULT_DIRECTORY", self.directory)
        self.config_patch = patch.object(tool, "DEFAULT_CONFIG", self.default)
        self.default_patch.start()
        self.config_patch.start()
        self.addCleanup(self.default_patch.stop)
        self.addCleanup(self.config_patch.stop)
        self.addCleanup(self.temporary.cleanup)

    def initialized(self):
        tool.Manager(self.paths).run("init")
        fake = FakeRPC(self.directory, self.old)
        return tool.Manager(self.paths, fake), fake

    def source(self, value=None):
        path = self.root / "candidate.json"
        path.write_bytes(tool.encode_json(self.new if value is None else value))
        return path

    def assert_config(self, path, expected):
        self.assertEqual(tool.read_config(path)[0], expected)

    def assert_clean_lock(self):
        self.assertFalse((self.directory / ".combat-config.lock").exists())

    def test_init_preserves_secrets_and_existing_config(self):
        original = self.env.read_bytes()
        first = tool.Manager(self.paths).run("init")
        self.assertTrue(first["created"])
        self.assertTrue(first["env_updated"])
        self.assertTrue(first["restart_required"])
        self.assertEqual(Path(first["env_backup"]).read_bytes(), original)
        self.assertEqual(self.env.read_bytes(), original.replace(b"COMBAT_CONFIG_FILE=\r\n", b"COMBAT_CONFIG_FILE=/app/config/current.json\r\n"))
        if os.name != "nt":
            self.assertEqual(self.directory.stat().st_mode & 0o777, 0o755)
            self.assertEqual((self.directory / "current.json").stat().st_mode & 0o777, 0o644)
            self.assertEqual(Path(first["env_backup"]).stat().st_mode & 0o777, 0o600)
        custom_bytes = b'{"version":"existing","anything":true}\n'
        (self.directory / "current.json").write_bytes(custom_bytes)
        second = tool.Manager(self.paths).run("init")
        self.assertFalse(second["created"])
        self.assertFalse(second["env_updated"])
        self.assertEqual((self.directory / "current.json").read_bytes(), custom_bytes)
        self.assertEqual(len(list(self.deploy.glob("*.bak.env"))), 1)
        self.assert_clean_lock()

    def test_init_adds_setting_without_rewriting_other_lines(self):
        self.env.write_bytes(b"INTERNAL_TOKEN=keep-no-newline")
        tool.Manager(self.paths).run("init")
        self.assertEqual(self.env.read_bytes(), b"INTERNAL_TOKEN=keep-no-newline\nCOMBAT_CONFIG_FILE=/app/config/current.json\n")

    def test_init_accepts_empty_quoted_or_commented_settings(self):
        for value in (b" # enable after init", b"'' # empty", b'"" # empty'):
            self.env.write_bytes(b"INTERNAL_TOKEN=keep\nCOMBAT_CONFIG_FILE=" + value + b"\n")
            result = tool.Manager(self.paths).run("init")
            self.assertTrue(result["env_updated"])
            self.assertIn(b"COMBAT_CONFIG_FILE=/app/config/current.json\n", self.env.read_bytes())

    def test_missing_env_does_not_create_directory_or_partial_env(self):
        self.env.unlink()
        with self.assertRaisesRegex(tool.ToolError, "实际 env"):
            tool.Manager(self.paths).run("init")
        self.assertFalse(self.directory.exists())
        self.assertFalse(self.env.exists())

    def test_init_refuses_unexpected_or_duplicate_env_without_changes(self):
        for settings in (b"COMBAT_CONFIG_FILE=/other/file.json\n", b"COMBAT_CONFIG_DIR=/unrelated\n",
                         b"COMBAT_CONFIG_FILE=\nCOMBAT_CONFIG_FILE=\n", b"COMBAT_CONFIG_DIR=${OTHER}\n"):
            with self.subTest(settings=settings):
                self.env.write_bytes(settings)
                with self.assertRaises(tool.ToolError):
                    tool.Manager(self.paths).run("init")
                self.assertEqual(self.env.read_bytes(), settings)
                self.assertFalse(self.directory.exists())
                self.assertEqual(list(self.deploy.glob("*.bak.env")), [])

    def test_custom_directory_requires_explicit_matching_env(self):
        custom = self.root / "custom-config"
        paths = tool.Paths(self.env, self.compose, custom)
        self.env.write_text(f"COMBAT_CONFIG_DIR={custom}\nCOMBAT_CONFIG_FILE=\n", encoding="utf-8")
        with self.assertRaisesRegex(tool.ToolError, "自定义目录"):
            tool.Manager(paths).run("init")
        self.assertFalse(custom.exists())
        self.env.write_text(f"COMBAT_CONFIG_DIR={custom}\nCOMBAT_CONFIG_FILE=/app/config/current.json\n", encoding="utf-8")
        result = tool.Manager(paths).run("init")
        self.assertTrue(result["created"])
        self.assertFalse(result["env_updated"])

    def test_publish_and_noop_keep_previous_and_generation(self):
        manager, fake = self.initialized()
        original = manager.current.read_bytes()

        def verify_before_validation(service, operation, payload):
            if operation == "validate":
                self.assertEqual(manager.current.read_bytes(), original)
                self.assertFalse(manager.previous.exists())

        fake.before = verify_before_validation
        first = manager.run("publish", self.source())
        fake.before = None
        self.assertTrue(first["changed"])
        self.assert_config(manager.current, self.new)
        self.assert_config(manager.previous, self.old)
        self.assertEqual([node["generation"] for node in fake.nodes.values()], [2, 2])
        previous_bytes = manager.previous.read_bytes()
        reload_count = sum(op == "reload" for _, op, _ in fake.calls)
        second = manager.run("publish", self.source())
        self.assertFalse(second["changed"])
        self.assertEqual(manager.previous.read_bytes(), previous_bytes)
        self.assertEqual(sum(op == "reload" for _, op, _ in fake.calls), reload_count)
        self.assertEqual([node["generation"] for node in fake.nodes.values()], [2, 2])
        self.assert_clean_lock()

    def test_rollback_uses_same_protocol_and_can_switch_back(self):
        manager, fake = self.initialized()
        manager.run("publish", self.source())
        self.assertEqual(manager.run("rollback")["version"], "test-v1")
        self.assert_config(manager.previous, self.new)
        self.assertEqual(manager.run("rollback")["version"], "test-v2")
        self.assert_config(manager.previous, self.old)
        self.assertEqual([node["generation"] for node in fake.nodes.values()], [4, 4])

    def test_noop_still_confirms_final_state_without_reload(self):
        manager, fake = self.initialized()
        validations = 0

        def external_update_after_validate(service, operation, payload, result):
            nonlocal validations
            if operation == "validate":
                validations += 1
                if validations == 2:
                    fake.nodes["scene1"].update(config=self.new, version=self.new["version"], generation=2)

        fake.after = external_update_after_validate
        with self.assertRaisesRegex(tool.ToolError, "幂等发布"):
            manager.run("publish", self.source(self.old))
        self.assertFalse(any(op == "reload" for _, op, _ in fake.calls))
        self.assert_config(manager.current, self.old)

    def test_status_reports_drift_and_publish_refuses(self):
        manager, fake = self.initialized()
        fake.nodes["scene2"]["config"] = config("external-v3")
        fake.nodes["scene2"]["version"] = "external-v3"
        self.assertFalse(manager.run("status")["consistent"])
        with self.assertRaisesRegex(tool.ToolError, "必须先与 current.json 一致"):
            manager.run("publish", self.source())
        self.assert_config(manager.current, self.old)
        self.assertFalse(manager.previous.exists())
        self.assertFalse(any(op == "reload" for _, op, _ in fake.calls))

    def test_validation_failure_or_normalization_difference_never_writes(self):
        for failure in ("reject", "normalization", "generation"):
            with self.subTest(failure=failure):
                manager, fake = self.initialized()
                if failure == "normalization":
                    fake.normalize["scene2"] = config("test-v2", 99)
                else:
                    def fail(service, operation, payload):
                        if service == "scene2" and operation == "validate":
                            if failure == "generation":
                                fake.nodes[service]["generation"] += 1
                            else:
                                raise tool.ToolError("HTTP 400 invalid config")
                    fake.before = fail
                with self.assertRaises(tool.ToolError):
                    manager.run("publish", self.source())
                self.assert_config(manager.current, self.old)
                self.assertFalse(manager.previous.exists())
                self.assertFalse(any(op == "reload" for _, op, _ in fake.calls))
                self.assert_clean_lock()

    def test_second_node_failure_restores_files_and_first_node(self):
        manager, fake = self.initialized()
        prior_previous = b'{"version":"older-v0","extra":true}\n'
        manager.previous.write_bytes(prior_previous)
        failed = False

        def fail_once(service, operation, payload):
            nonlocal failed
            if service == "scene2" and operation == "reload" and not failed:
                failed = True
                raise tool.ToolError("HTTP 503 test failure")

        fake.before = fail_once
        with self.assertRaises(tool.ToolError) as caught:
            manager.run("publish", self.source())
        self.assertFalse(caught.exception.recovery["manual_recovery_required"])
        self.assertTrue(caught.exception.recovery["nodes"]["scene1"]["compensated"])
        self.assert_config(manager.current, self.old)
        self.assertEqual(manager.previous.read_bytes(), prior_previous)
        self.assertEqual([node["config"] for node in fake.nodes.values()], [self.old, self.old])
        self.assertEqual([node["generation"] for node in fake.nodes.values()], [3, 1])
        self.assert_clean_lock()

    def test_timeout_after_apply_is_compensated_by_known_generation(self):
        manager, fake = self.initialized()
        timed_out = False

        def timeout_after_apply(service, operation, payload, result):
            nonlocal timed_out
            if operation == "reload" and not timed_out:
                timed_out = True
                raise tool.ToolError("operation timed out after applying")

        fake.after = timeout_after_apply
        with self.assertRaises(tool.ToolError) as caught:
            manager.run("publish", self.source())
        self.assertFalse(caught.exception.recovery["manual_recovery_required"])
        self.assertFalse(manager.previous.exists())
        self.assert_config(manager.current, self.old)
        compensation = [payload for service, op, payload in fake.calls if service == "scene1" and op == "reload"]
        self.assertEqual(compensation, [{"expected_generation": 1}, {"expected_generation": 2}])

    def test_unknown_concurrent_version_is_never_overwritten(self):
        manager, fake = self.initialized()
        external = config("external-v8", 80)

        def concurrent_update(service, operation, payload):
            if service == "scene2" and operation == "reload":
                fake.nodes[service].update(config=external, version=external["version"], generation=8)
                raise tool.ToolError("HTTP 409 concurrent update")

        fake.before = concurrent_update
        with self.assertRaises(tool.ToolError) as caught:
            manager.run("publish", self.source())
        self.assertTrue(caught.exception.recovery["manual_recovery_required"])
        self.assertFalse(caught.exception.recovery["nodes"]["scene2"]["restored"])
        self.assertEqual(fake.nodes["scene2"]["config"], external)
        self.assertEqual(fake.nodes["scene1"]["config"], self.old)
        self.assert_config(manager.current, self.old)

    def test_compensation_also_uses_cas_against_racing_update(self):
        manager, fake = self.initialized()
        external = config("external-v3", 77)

        def race(service, operation, payload):
            if service == "scene2" and operation == "reload":
                raise tool.ToolError("reload failure")
            if service == "scene1" and operation == "reload" and payload["expected_generation"] == 2:
                fake.nodes[service].update(config=external, version=external["version"], generation=3)

        fake.before = race
        with self.assertRaises(tool.ToolError) as caught:
            manager.run("publish", self.source())
        self.assertTrue(caught.exception.recovery["manual_recovery_required"])
        self.assertEqual(fake.nodes["scene1"]["config"], external)

    def test_concurrent_disk_change_is_not_overwritten_during_recovery(self):
        manager, fake = self.initialized()
        external = config("external-file-v3", 88)

        def race(service, operation, payload):
            if service == "scene2" and operation == "reload":
                manager.current.write_bytes(tool.encode_json(external))
                raise tool.ToolError("reload failure")

        fake.before = race
        with self.assertRaises(tool.ToolError) as caught:
            manager.run("publish", self.source())
        self.assertTrue(caught.exception.recovery["manual_recovery_required"])
        self.assertFalse(caught.exception.recovery["current_restored"])
        self.assert_config(manager.current, external)
        self.assertEqual(fake.nodes["scene1"]["config"], self.new)

    def test_partial_file_write_failure_restores_previous(self):
        manager, fake = self.initialized()
        real_write = tool.atomic_write
        failed = False

        def fail_current(path, data, mode=0o644):
            nonlocal failed
            if path == manager.current and not failed:
                failed = True
                raise OSError("test disk full")
            return real_write(path, data, mode)

        with patch.object(tool, "atomic_write", side_effect=fail_current):
            with self.assertRaises(tool.ToolError) as caught:
                manager.run("publish", self.source())
        self.assertFalse(caught.exception.recovery["manual_recovery_required"])
        self.assert_config(manager.current, self.old)
        self.assertFalse(manager.previous.exists())
        self.assertFalse(any(op == "reload" for _, op, _ in fake.calls))

    def test_existing_lock_and_replaced_lock_are_not_deleted(self):
        manager, _ = self.initialized()
        lock = self.directory / ".combat-config.lock"
        lock.write_bytes(b"other-owner")
        with self.assertRaisesRegex(tool.ToolError, "已锁定"):
            manager.run("status")
        self.assertEqual(lock.read_bytes(), b"other-owner")
        lock.unlink()
        with tool.exclusive_lock(self.directory):
            lock.unlink()
            lock.write_bytes(b"replaced-owner")
        self.assertEqual(lock.read_bytes(), b"replaced-owner")

    def test_atomic_replace_failure_leaves_original_and_no_temp(self):
        self.directory.mkdir()
        path = self.directory / "current.json"
        path.write_bytes(b"original")
        with patch.object(tool.os, "replace", side_effect=OSError("replace failed")):
            with self.assertRaises(OSError):
                tool.atomic_write(path, b"replacement")
        self.assertEqual(path.read_bytes(), b"original")
        self.assertEqual(list(self.directory.iterdir()), [path])

    def test_invalid_json_and_missing_previous_are_rejected(self):
        manager, _ = self.initialized()
        with self.assertRaises(tool.ToolError):
            manager.run("rollback")
        for data in (b'{"version":"a","version":"b"}', b'{"version":"a","damage":NaN}', b"[1]", b" " * 65537):
            with self.subTest(data=data[:80]):
                with self.assertRaises(tool.ToolError):
                    tool.decode_config(data)

    def test_docker_rpc_uses_constant_endpoint_stdin_and_container_token(self):
        reply = subprocess.CompletedProcess([], 0, b'{"valid":true}\n200', b"")
        with patch.object(tool.subprocess, "run", return_value=reply) as run:
            result = tool.DockerRPC(self.paths)("scene2", "validate", {"config": self.new, "expected_generation": 7})
        self.assertEqual(result, {"valid": True})
        args, kwargs = run.call_args
        command = args[0]
        self.assertEqual(command[-5:-2], ["-T", "scene2", "sh"])
        self.assertEqual(command[-1], tool.RPC_COMMANDS["validate"])
        self.assertIn("$INTERNAL_TOKEN", command[-1])
        self.assertNotIn("local-test-secret", str(command))
        self.assertNotIn("test-v2", str(command))
        self.assertEqual(json.loads(kwargs["input"]), {"config": self.new, "expected_generation": 7})
        self.assertEqual(kwargs["timeout"], 25)
        self.assertIn("--project-name", command)

    def test_docker_errors_do_not_echo_stderr_or_response_secrets(self):
        responses = [subprocess.CompletedProcess([], 1, b"", b"sensitive-secret"),
                     subprocess.CompletedProcess([], 0, b'{"error":"sensitive-secret"}\n401', b""),
                     subprocess.CompletedProcess([], 0, b"not-json\n200", b"")]
        for response in responses:
            with self.subTest(response=response):
                with patch.object(tool.subprocess, "run", return_value=response):
                    with self.assertRaises(tool.ToolError) as caught:
                        tool.DockerRPC(self.paths)("scene1", "status", {})
                self.assertNotIn("sensitive-secret", str(caught.exception))
        with patch.object(tool.subprocess, "run", side_effect=subprocess.TimeoutExpired("docker", 25)):
            with self.assertRaisesRegex(tool.ToolError, "超时"):
                tool.DockerRPC(self.paths)("scene1", "reload", {"expected_generation": 1})

    def test_main_emits_one_json_result_and_public_parameters_precede_command(self):
        options = ["--env-file", str(self.env), "--compose-file", str(self.compose), "--directory", str(self.directory)]
        output = io.StringIO()
        with redirect_stdout(output):
            code = tool.main(options + ["init"])
        self.assertEqual(code, 0)
        self.assertEqual(len(output.getvalue().splitlines()), 1)
        parsed = json.loads(output.getvalue())
        self.assertTrue(parsed["ok"])
        self.assertEqual(parsed["command"], "init")
        self.assertNotIn("local-test-secret", output.getvalue())
        self.env.unlink()
        output = io.StringIO()
        with redirect_stdout(output):
            self.assertEqual(tool.main(options + ["status"]), 1)
        self.assertFalse(json.loads(output.getvalue())["ok"])
        output = io.StringIO()
        with redirect_stdout(output):
            self.assertEqual(tool.main(["status", "--directory", str(self.directory)]), 1)
        self.assertFalse(json.loads(output.getvalue())["ok"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
