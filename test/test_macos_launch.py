#!/usr/bin/env python3

import json
import os
import pathlib
import shutil
import socket
import subprocess
import tempfile
import time
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
DMTCP_COMMAND = ROOT / "bin" / "dmtcp_command"
DMTCP_COORDINATOR = ROOT / "bin" / "dmtcp_coordinator"
DMTCP_LAUNCH = ROOT / "bin" / "dmtcp_launch"
COMMAND_TIMEOUT = 10


def wait_until(predicate, description, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {description}")


class MacOSLaunchTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if os.uname().sysname != "Darwin":
            raise unittest.SkipTest("macOS launch test requires Darwin")

        cls.temp = tempfile.TemporaryDirectory(prefix="dmtcp-macos-launch-")
        cls.temp_path = pathlib.Path(cls.temp.name)
        source = cls.temp_path / "fixture.c"
        cls.fixture = cls.temp_path / "fixture"
        source.write_text(
            "#include <stdio.h>\n"
            "#include <stdlib.h>\n"
            "#include <unistd.h>\n"
            "int main(int argc, char **argv) {\n"
            "  puts(\"fixture-main\");\n"
            "  fflush(stdout);\n"
            "  sleep(argc > 1 ? (unsigned)atoi(argv[1]) : 30);\n"
            "  return 0;\n"
            "}\n",
            encoding="utf-8",
        )
        subprocess.run(
            ["cc", str(source), "-o", str(cls.fixture)],
            check=True,
            cwd=ROOT,
            timeout=COMMAND_TIMEOUT,
        )

        fork_source = cls.temp_path / "fork_fixture.c"
        cls.fork_fixture = cls.temp_path / "fork_fixture"
        fork_source.write_text(
            "#include <errno.h>\n"
            "#include <stdio.h>\n"
            "#include <unistd.h>\n"
            "int main(void) {\n"
            "  pid_t child = fork();\n"
            "  puts(child == -1 && errno == ENOTSUP\n"
            "         ? \"fork-blocked\" : \"fork-escaped\");\n"
            "  fflush(stdout);\n"
            "  sleep(30);\n"
            "  return 0;\n"
            "}\n",
            encoding="utf-8",
        )
        subprocess.run(
            ["cc", str(fork_source), "-o", str(cls.fork_fixture)],
            check=True,
            cwd=ROOT,
            timeout=COMMAND_TIMEOUT,
        )

        exec_source = cls.temp_path / "exec_fixture.c"
        cls.exec_fixture = cls.temp_path / "exec_fixture"
        exec_source.write_text(
            "#include <errno.h>\n"
            "#include <stdio.h>\n"
            "#include <unistd.h>\n"
            "int main(void) {\n"
            "  int result = execl(\"/bin/sleep\", \"sleep\", \"30\", "
            "(char *)0);\n"
            "  puts(result == -1 && errno == ENOTSUP\n"
            "         ? \"exec-blocked\" : \"exec-escaped\");\n"
            "  fflush(stdout);\n"
            "  sleep(30);\n"
            "  return 0;\n"
            "}\n",
            encoding="utf-8",
        )
        subprocess.run(
            ["cc", str(exec_source), "-o", str(cls.exec_fixture)],
            check=True,
            cwd=ROOT,
            timeout=COMMAND_TIMEOUT,
        )

        restricted_source = cls.temp_path / "restricted_fixture.c"
        cls.restricted_fixture = cls.temp_path / "restricted_fixture"
        restricted_source.write_text(
            "#include <stdio.h>\n"
            "__attribute__((used, section(\"__RESTRICT,__restrict\")))\n"
            "static const char restricted[] = \"\";\n"
            "int main(void) { puts(\"restricted-main\"); return 0; }\n",
            encoding="utf-8",
        )
        subprocess.run(
            ["cc", str(restricted_source), "-o", str(cls.restricted_fixture)],
            check=True,
            cwd=ROOT,
            timeout=COMMAND_TIMEOUT,
        )

        cls.synthetic_worker = cls.temp_path / "coordinator_synthetic_worker"
        subprocess.run(
            [
                "g++",
                "-std=c++20",
                "-DDMTCP_CONTROL_ONLY",
                "-I./src",
                "-I./include",
                "-I./jalib",
                "test/coordinator_synthetic_worker.cpp",
                "src/libdmtcpcontrol.a",
                "src/libjalib.a",
                "-lpthread",
                "-o",
                str(cls.synthetic_worker),
            ],
            check=True,
            cwd=ROOT,
            timeout=COMMAND_TIMEOUT,
        )

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.port_file = self.temp_path / f"port-{time.time_ns()}"
        self.coordinator = subprocess.Popen(
            [
                str(DMTCP_COORDINATOR),
                "--quiet",
                "--coord-port",
                "0",
                "--port-file",
                str(self.port_file),
                "--timeout",
                "30",
            ],
            cwd=ROOT,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
        self.port = wait_until(self.read_port, "coordinator port file")
        self.target = None
        self.synthetic = None

    def tearDown(self):
        if self.synthetic is not None and self.synthetic.poll() is None:
            self.synthetic.kill()
            self.synthetic.wait(timeout=5)
        if self.synthetic is not None:
            if self.synthetic.stdout:
                self.synthetic.stdout.close()
            if self.synthetic.stderr:
                self.synthetic.stderr.close()
        if self.target is not None and self.target.poll() is None:
            self.target.kill()
            self.target.wait(timeout=5)
        if self.target is not None:
            if self.target.stdout:
                self.target.stdout.close()
            if self.target.stderr:
                self.target.stderr.close()
        if self.coordinator.poll() is None:
            self.run_command("--quit")
            try:
                self.coordinator.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.coordinator.kill()
                self.coordinator.wait(timeout=5)
        if self.coordinator.stderr:
            self.coordinator.stderr.close()
        self.port_file.unlink(missing_ok=True)

    def read_port(self):
        if not self.port_file.exists():
            return None
        text = self.port_file.read_text(encoding="utf-8").strip()
        return int(text) if text else None

    def run_command(self, command):
        return subprocess.run(
            [
                str(DMTCP_COMMAND),
                "--json",
                "--coord-host",
                "127.0.0.1",
                "--coord-port",
                str(self.port),
                command,
            ],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=COMMAND_TIMEOUT,
            check=False,
        )

    def status(self):
        result = self.run_command("--status")
        if result.returncode != 0:
            return None
        return json.loads(result.stdout)

    def launch_fixture(self, hold_seconds=30):
        self.target = subprocess.Popen(
            [
                str(DMTCP_LAUNCH),
                "--quiet",
                "--join-coordinator",
                "--coord-host",
                "127.0.0.1",
                "--coord-port",
                str(self.port),
                "--",
                str(self.fixture),
                str(hold_seconds),
            ],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def wait_for_peer_count(self, count):
        return wait_until(
            lambda: (payload if (payload := self.status())
                     and payload["num_peers"] == count else None),
            f"coordinator peer count {count}",
        )

    def test_launch_registers_and_rejects_all_checkpoint_commands(self):
        self.launch_fixture()
        payload = self.wait_for_peer_count(1)
        self.assertTrue(payload["running"])
        self.assertEqual(self.target.stdout.readline().strip(), "fixture-main")

        for command in ("--checkpoint", "--bcheckpoint", "--kcheckpoint"):
            with self.subTest(command=command):
                result = self.run_command(command)
                self.assertEqual(result.returncode, 2, result.stderr)
                response = json.loads(result.stdout)
                self.assertEqual(
                    response["command_status"],
                    "DMT_COORD_CKPT_UNSUPPORTED",
                )
                self.assertIsNone(self.target.poll())

        result = self.run_command("--kill")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.target.wait(timeout=5)
        self.assertEqual(self.target.returncode, 0)
        self.wait_for_peer_count(0)

    def test_registration_is_rejected_during_active_checkpoint(self):
        self.synthetic = subprocess.Popen(
            [
                str(self.synthetic_worker),
                "127.0.0.1",
                str(self.port),
                "--expect-checkpoint",
                "--hold-seconds",
                "30",
            ],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.assertIn("accepted virtual_pid=", self.synthetic.stdout.readline())
        self.wait_for_peer_count(1)

        checkpoint = self.run_command("--checkpoint")
        self.assertEqual(checkpoint.returncode, 0, checkpoint.stderr)
        self.assertEqual(
            self.synthetic.stdout.readline().strip(),
            "received DMT_DO_CHECKPOINT",
        )

        result = subprocess.run(
            [
                str(DMTCP_LAUNCH),
                "--quiet",
                "--coord-host",
                "127.0.0.1",
                "--coord-port",
                str(self.port),
                "--",
                str(self.fixture),
                "0",
            ],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=COMMAND_TIMEOUT,
            check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("fixture-main", result.stdout)
        self.assertIn("rejected registration-only worker", result.stderr)
        self.assertEqual(self.status()["num_peers"], 1)

    def test_exec_is_blocked(self):
        self.target = subprocess.Popen(
            [
                str(DMTCP_LAUNCH),
                "--quiet",
                "--coord-host",
                "127.0.0.1",
                "--coord-port",
                str(self.port),
                "--",
                str(self.exec_fixture),
            ],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.wait_for_peer_count(1)
        self.assertEqual(self.target.stdout.readline().strip(), "exec-blocked")
        result = self.run_command("--kill")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.target.wait(timeout=5)
        self.assertEqual(self.target.returncode, 0)
        self.wait_for_peer_count(0)

    def test_process_creation_is_blocked(self):
        self.target = subprocess.Popen(
            [
                str(DMTCP_LAUNCH),
                "--quiet",
                "--coord-host",
                "127.0.0.1",
                "--coord-port",
                str(self.port),
                "--",
                str(self.fork_fixture),
            ],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.wait_for_peer_count(1)
        self.assertEqual(self.target.stdout.readline().strip(), "fork-blocked")
        result = self.run_command("--kill")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.target.wait(timeout=5)
        self.assertEqual(self.target.returncode, 0)
        self.wait_for_peer_count(0)

    def test_natural_exit_unregisters_process(self):
        self.launch_fixture(1)
        self.wait_for_peer_count(1)
        self.target.wait(timeout=5)
        self.assertEqual(self.target.returncode, 0)
        self.assertEqual(self.target.stdout.readline().strip(), "fixture-main")
        self.wait_for_peer_count(0)

    def test_help_and_version(self):
        help_result = subprocess.run(
            [str(DMTCP_LAUNCH), "--help"],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=COMMAND_TIMEOUT,
            check=False,
        )
        self.assertEqual(help_result.returncode, 0, help_result.stderr)
        self.assertIn("register", help_result.stdout)

        version_result = subprocess.run(
            [str(DMTCP_LAUNCH), "--version"],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=COMMAND_TIMEOUT,
            check=False,
        )
        self.assertEqual(version_result.returncode, 0, version_result.stderr)
        self.assertIn("dmtcp_launch (DMTCP)", version_result.stdout)

        protected_result = subprocess.run(
            [str(DMTCP_LAUNCH), "--quiet", "--", "/bin/echo", "unsafe"],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=COMMAND_TIMEOUT,
            check=False,
        )
        self.assertEqual(protected_result.returncode, 2)
        self.assertNotIn("unsafe", protected_result.stdout)
        self.assertIn("platform-protected", protected_result.stderr)

    def test_dyld_restricted_target_is_rejected_before_main(self):
        result = subprocess.run(
            [str(DMTCP_LAUNCH), "--quiet", "--",
             str(self.restricted_fixture)],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=COMMAND_TIMEOUT,
            check=False,
        )
        self.assertEqual(result.returncode, 2)
        self.assertNotIn("restricted-main", result.stdout)
        self.assertIn("dyld-restricted", result.stderr)

    def test_hardened_target_is_rejected_before_main(self):
        hardened = self.temp_path / f"hardened-{time.time_ns()}"
        shutil.copy2(self.fixture, hardened)
        self.addCleanup(hardened.unlink, missing_ok=True)
        subprocess.run(
            ["codesign", "--force", "--sign", "-", "--options", "runtime",
             str(hardened)],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=COMMAND_TIMEOUT,
            check=True,
        )
        result = subprocess.run(
            [str(DMTCP_LAUNCH), "--quiet", "--", str(hardened), "0"],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=COMMAND_TIMEOUT,
            check=False,
        )
        self.assertEqual(result.returncode, 2)
        self.assertNotIn("fixture-main", result.stdout)
        self.assertIn("hardened", result.stderr)

    def test_registration_failure_stops_before_main(self):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.bind(("127.0.0.1", 0))
            unused_port = probe.getsockname()[1]
        result = subprocess.run(
            [
                str(DMTCP_LAUNCH),
                "--quiet",
                "--coord-host",
                "127.0.0.1",
                "--coord-port",
                str(unused_port),
                "--",
                str(self.fixture),
                "0",
            ],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=COMMAND_TIMEOUT,
            check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("fixture-main", result.stdout)
        self.assertIn("cannot connect to coordinator", result.stderr)


if __name__ == "__main__":
    unittest.main()
