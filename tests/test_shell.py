#!/usr/bin/env python3
"""Run behavioral checks against the compiled learning shell on Linux.

Usage: python3 tests/test_shell.py [path/to/myshell]
The default binary is ../myshell, relative to this test file.

All command files live in temporary directories. Ctrl+Z is deliberately outside
this suite: handling stopped foreground children needs a documented policy
before it can be tested without accidentally leaving a stopped process behind.
"""

import errno
import os
from pathlib import Path
import select
import shlex
import signal
import subprocess
import sys
import tempfile
import time
import unittest


BINARY = Path(__file__).resolve().parent.parent / "myshell"
if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
    BINARY = Path(sys.argv.pop(1)).resolve()


@unittest.skipUnless(sys.platform.startswith("linux"), "requires Linux")
class ShellBehaviorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="myshell-test-")
        self.addCleanup(self.temp.cleanup)
        self.cwd = Path(self.temp.name)
        self.env = os.environ.copy()
        self.env.update(HOME=str(self.cwd), LC_ALL="C")
        self.assertTrue(BINARY.is_file(), f"shell binary does not exist: {BINARY}")

    def run_shell(self, commands, *, env=None, timeout=5):
        process = subprocess.Popen(
            [str(BINARY)],
            cwd=self.cwd,
            env=self.env if env is None else env,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
        try:
            stdout, stderr = process.communicate(commands, timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            stdout, stderr = process.communicate()
            self.fail(
                f"shell timed out after {timeout}s; possible pipeline deadlock\n"
                f"stdout={stdout!r}\nstderr={stderr!r}"
            )
        return subprocess.CompletedProcess(
            [str(BINARY)], process.returncode, stdout, stderr
        )

    def assert_ok(self, result, stdout=None):
        self.assertEqual(result.returncode, 0, result.stderr)
        if stdout is not None:
            self.assertEqual(result.stdout, stdout)

    def test_empty_input_and_blank_lines(self):
        for commands in ("", "\n\n   \t\n"):
            with self.subTest(commands=commands):
                self.assert_ok(self.run_shell(commands), "")

    def test_noninteractive_mode_has_no_prompt(self):
        result = self.run_shell("printf 'hello\\n'\n")
        self.assert_ok(result, "hello\n")
        self.assertEqual(result.stderr, "")

    def test_eof_executes_last_line_without_newline(self):
        self.assert_ok(self.run_shell("printf eof"), "eof")

    def test_external_command_can_consume_remaining_stdin(self):
        self.assert_ok(self.run_shell("cat\nhello-from-stdin\nanother-line\n"),
                       "hello-from-stdin\nanother-line\n")

    def test_long_line_is_not_truncated(self):
        word = "abcdef" * 1000
        self.assert_ok(self.run_shell(f"printf '%s' '{word}'\n"), word)

    def test_more_than_sixty_three_arguments(self):
        arguments = [f"arg{i}" for i in range(150)]
        result = self.run_shell("printf '%s\\n' " + " ".join(arguments) + "\n")
        self.assert_ok(result, "".join(argument + "\n" for argument in arguments))

    def test_external_command_is_searched_on_path(self):
        self.assert_ok(self.run_shell("printf '%s\\n' PATH_OK\n"), "PATH_OK\n")

    def test_missing_command_returns_127(self):
        result = self.run_shell("__myshell_command_that_does_not_exist__\n")
        self.assertEqual(result.returncode, 127)
        self.assertNotEqual(result.stderr, "")

    def test_nonexecutable_command_returns_126(self):
        command = self.cwd / "not-executable"
        command.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        command.chmod(0o644)
        result = self.run_shell("./not-executable\n")
        self.assertEqual(result.returncode, 126)
        self.assertNotEqual(result.stderr, "")

    def test_last_command_status_is_returned_at_eof(self):
        self.assertEqual(self.run_shell("true\nfalse\n").returncode, 1)
        self.assertEqual(self.run_shell("false\ntrue\n").returncode, 0)

    def test_exit_without_argument_preserves_last_status(self):
        result = self.run_shell("false\nexit\nprintf should-not-run\n")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")

    def test_numeric_exit_status_wraps_to_eight_bits(self):
        for supplied, expected in ((0, 0), (1, 1), (42, 42), (255, 255),
                                   (256, 0), (257, 1), (-1, 255), (-256, 0)):
            with self.subTest(supplied=supplied):
                result = self.run_shell(f"exit {supplied}\nprintf should-not-run\n")
                self.assertEqual(result.returncode, expected, result.stderr)
                self.assertEqual(result.stdout, "")

    def test_nonnumeric_exit_exits_with_status_two(self):
        result = self.run_shell("exit invalid\nprintf should-not-run\n")
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "")
        self.assertNotEqual(result.stderr, "")

    def test_exit_with_too_many_arguments_does_not_exit(self):
        result = self.run_shell("exit 3 4\nprintf alive\n")
        self.assert_ok(result, "alive")
        self.assertNotEqual(result.stderr, "")
        self.assertEqual(self.run_shell("exit 3 4\n").returncode, 2)

    def test_pwd(self):
        self.assert_ok(self.run_shell("pwd\n"), str(self.cwd) + "\n")

    def test_cd_changes_parent_shell_directory(self):
        child = self.cwd / "directory with spaces"
        child.mkdir()
        self.assert_ok(self.run_shell("cd 'directory with spaces'\npwd\n"),
                       str(child) + "\n")

    def test_cd_without_argument_uses_home(self):
        home = self.cwd / "home-directory"
        home.mkdir()
        self.env["HOME"] = str(home)
        self.assert_ok(self.run_shell("cd\npwd\n"), str(home) + "\n")

    def test_cd_without_home_is_an_error(self):
        env = self.env.copy()
        env.pop("HOME", None)
        result = self.run_shell("cd\n", env=env)
        self.assertEqual(result.returncode, 1)
        self.assertNotEqual(result.stderr, "")
        result = self.run_shell("cd\npwd\n", env=env)
        self.assert_ok(result, str(self.cwd) + "\n")

    def test_failed_cd_preserves_directory(self):
        self.assertEqual(self.run_shell("cd missing-directory\n").returncode, 1)
        result = self.run_shell("cd missing-directory\npwd\n")
        self.assert_ok(result, str(self.cwd) + "\n")
        self.assertNotEqual(result.stderr, "")

    def test_cd_rejects_extra_arguments(self):
        (self.cwd / "one").mkdir()
        (self.cwd / "two").mkdir()
        self.assertNotEqual(self.run_shell("cd one two\n").returncode, 0)
        self.assert_ok(self.run_shell("cd one two\npwd\n"), str(self.cwd) + "\n")

    def test_quotes_empty_arguments_and_backslash(self):
        commands = (
            "printf '<%s>\\n' 'single words' \"double words\" '' \"\" "
            "escaped\\ space pre'joined'post '|' \"<\" '>' \\| \"a\\\"b\"\n"
        )
        expected = "<single words>\n<double words>\n<>\n<>\n<escaped space>\n"
        expected += "<prejoinedpost>\n<|>\n<<>\n<>>\n<|>\n<a\"b>\n"
        self.assert_ok(self.run_shell(commands), expected)

    def test_unclosed_quote_is_syntax_error_and_shell_recovers(self):
        for bad in ("printf 'unfinished", 'printf "unfinished'):
            with self.subTest(command=bad):
                result = self.run_shell(bad + "\n")
                self.assertEqual(result.returncode, 2)
                self.assertEqual(result.stdout, "")
                self.assertNotEqual(result.stderr, "")
                result = self.run_shell(bad + "\nprintf recovered\n")
                self.assert_ok(result, "recovered")

    def test_embedded_nul_is_rejected_and_shell_recovers(self):
        commands = "printf should-not-run\x00ignored\n"
        result = self.run_shell(commands)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "")
        self.assertNotEqual(result.stderr, "")
        self.assert_ok(self.run_shell(commands + "printf recovered\n"), "recovered")

    def test_backslash_newline_is_rejected_without_partial_execution(self):
        commands = "printf should-not-run\\\n"
        result = self.run_shell(commands)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "")
        self.assertNotEqual(result.stderr, "")
        self.assert_ok(self.run_shell(commands + "printf recovered\n"), "recovered")

    def test_trailing_backslash_at_eof_is_rejected(self):
        result = self.run_shell("printf should-not-run\\")
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "")
        self.assertNotEqual(result.stderr, "")

    def test_multistage_pipeline(self):
        commands = "printf 'pear\\napple\\npear\\n'|sort|uniq|wc -l\n"
        result = self.run_shell(commands)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "2")

    def test_pipeline_returns_last_command_status(self):
        self.assertEqual(self.run_shell("false | true\n").returncode, 0)
        self.assertEqual(self.run_shell("true | false\n").returncode, 1)

    def test_output_input_and_append_redirection_without_spaces(self):
        commands = "printf first>data.txt\nprintf second>>data.txt\ncat<data.txt\n"
        self.assert_ok(self.run_shell(commands), "firstsecond")
        self.assertEqual((self.cwd / "data.txt").read_text(), "firstsecond")

    def test_redirection_accepts_quoted_filename(self):
        result = self.run_shell("printf contents>'file with spaces'\ncat<'file with spaces'\n")
        self.assert_ok(result, "contents")

    def test_output_redirection_truncates_existing_file(self):
        target = self.cwd / "data"
        target.write_text("old long contents", encoding="utf-8")
        self.assert_ok(self.run_shell("printf new>data\n"), "")
        self.assertEqual(target.read_text(), "new")

    def test_redirection_may_precede_command(self):
        (self.cwd / "input").write_text("before-command", encoding="utf-8")
        self.assert_ok(self.run_shell("<input cat >output\n"), "")
        self.assertEqual((self.cwd / "output").read_text(), "before-command")

    def test_last_redirection_wins_in_source_order(self):
        commands = "printf payload>first>second\n"
        self.assert_ok(self.run_shell(commands), "")
        self.assertEqual((self.cwd / "first").read_text(), "")
        self.assertEqual((self.cwd / "second").read_text(), "payload")
        (self.cwd / "input-one").write_text("one", encoding="utf-8")
        (self.cwd / "input-two").write_text("two", encoding="utf-8")
        self.assert_ok(self.run_shell("cat<input-one<input-two\n"), "two")

    def test_open_errors_follow_redirection_order(self):
        result = self.run_shell("cat >created <missing\n")
        self.assertEqual(result.returncode, 1)
        self.assertTrue((self.cwd / "created").exists())
        result = self.run_shell("cat <missing >not-created\n")
        self.assertEqual(result.returncode, 1)
        self.assertFalse((self.cwd / "not-created").exists())

    def test_output_redirection_overrides_pipe_destination(self):
        result = self.run_shell("printf payload >out | wc -c\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "0")
        self.assertEqual((self.cwd / "out").read_text(), "payload")

    def test_input_redirection_overrides_pipe_source(self):
        (self.cwd / "input").write_text("from-file", encoding="utf-8")
        self.assert_ok(self.run_shell("printf from-pipe | cat <input\n"), "from-file")

    def test_builtin_output_redirection_is_restored(self):
        commands = "pwd>saved\npwd\n"
        self.assert_ok(self.run_shell(commands), str(self.cwd) + "\n")
        self.assertEqual((self.cwd / "saved").read_text(), str(self.cwd) + "\n")

    def test_builtin_input_redirection_is_restored(self):
        (self.cwd / "input").write_text("ignored", encoding="utf-8")
        self.assert_ok(self.run_shell("pwd<input\nprintf next-command\n"),
                       str(self.cwd) + "\nnext-command")

    def test_failed_builtin_redirection_restores_descriptors(self):
        result = self.run_shell("pwd >discarded <missing\npwd\n")
        self.assert_ok(result, str(self.cwd) + "\n")
        self.assertEqual((self.cwd / "discarded").read_text(), "")
        self.assertNotEqual(result.stderr, "")

    def test_cd_in_pipeline_does_not_change_parent_directory(self):
        (self.cwd / "child").mkdir()
        self.assert_ok(self.run_shell("cd child | cat\npwd\n"), str(self.cwd) + "\n")

    def test_exit_in_pipeline_does_not_exit_parent(self):
        self.assert_ok(self.run_shell("exit 7 | cat\nprintf alive\n"), "alive")
        self.assertEqual(self.run_shell("true | exit 7\n").returncode, 7)

    def test_large_pipeline_runs_concurrently(self):
        code = "import sys; sys.stdout.write('x' * 1048576)"
        commands = f"python3 -c {shlex.quote(code)} | cat | wc -c\n"
        result = self.run_shell(commands, timeout=8)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "1048576")

    def test_early_pipeline_reader_exit_does_not_hang(self):
        self.assert_ok(self.run_shell("yes | head -n 1\n"), "y\n")

    def test_malformed_pipeline_does_not_execute_any_command(self):
        for bad in ("| printf x", "printf x |", "printf x || cat", "printf x | | cat"):
            with self.subTest(command=bad):
                result = self.run_shell(bad + "\n")
                self.assertEqual(result.returncode, 2)
                self.assertEqual(result.stdout, "")
                self.assertNotEqual(result.stderr, "")
        result = self.run_shell("printf x >should-not-exist |\n")
        self.assertEqual(result.returncode, 2)
        self.assertFalse((self.cwd / "should-not-exist").exists())

    def test_missing_redirection_operand_is_rejected_before_execution(self):
        for bad in ("printf x >", "cat <", "printf x >>", "printf x > | cat"):
            with self.subTest(command=bad):
                result = self.run_shell(bad + "\n")
                self.assertEqual(result.returncode, 2)
                self.assertEqual(result.stdout, "")
        result = self.run_shell("printf x >should-not-exist >\n")
        self.assertEqual(result.returncode, 2)
        self.assertFalse((self.cwd / "should-not-exist").exists())

    def test_unsupported_operators_are_rejected(self):
        for bad in ("printf x &", "printf x; printf y", "cat <<end", "printf x 2>error-file"):
            with self.subTest(command=bad):
                result = self.run_shell(bad + "\n")
                self.assertEqual(result.returncode, 2)
                self.assertEqual(result.stdout, "")
                self.assertNotEqual(result.stderr, "")
        self.assertFalse((self.cwd / "error-file").exists())

    def test_redirection_open_failure_returns_one(self):
        for commands in ("cat <missing\n", "printf x >missing-dir/output\n"):
            with self.subTest(commands=commands):
                result = self.run_shell(commands)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(result.stdout, "")
                self.assertNotEqual(result.stderr, "")


@unittest.skipUnless(sys.platform.startswith("linux"), "requires Linux PTY")
class InteractiveSignalTests(unittest.TestCase):
    def setUp(self):
        import pty
        import termios

        self.temp = tempfile.TemporaryDirectory(prefix="myshell-pty-test-")
        self.addCleanup(self.temp.cleanup)
        self.assertTrue(BINARY.is_file(), f"shell binary does not exist: {BINARY}")
        self.pid, self.master = pty.fork()
        if self.pid == 0:
            os.chdir(self.temp.name)
            env = os.environ.copy()
            env.update(HOME=self.temp.name, LC_ALL="C")
            os.execve(str(BINARY), [str(BINARY)], env)
        self.reaped = False
        self.addCleanup(self.cleanup_pty)
        attributes = termios.tcgetattr(self.master)
        attributes[3] &= ~termios.ECHO
        termios.tcsetattr(self.master, termios.TCSANOW, attributes)
        self.buffer = b""
        self.read_for(0.3)

    def cleanup_pty(self):
        if not self.reaped:
            try:
                os.killpg(self.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                os.waitpid(self.pid, 0)
            except ChildProcessError:
                pass
        os.close(self.master)

    def read_for(self, duration):
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            readable, _, _ = select.select([self.master], [], [], deadline - time.monotonic())
            if not readable:
                break
            try:
                chunk = os.read(self.master, 4096)
            except OSError as error:
                if error.errno == errno.EIO:
                    break
                raise
            if not chunk:
                break
            self.buffer += chunk

    def expect(self, marker, timeout=5):
        deadline = time.monotonic() + timeout
        while marker not in self.buffer and time.monotonic() < deadline:
            self.read_for(min(0.1, max(0, deadline - time.monotonic())))
        self.assertIn(marker, self.buffer, f"PTY output: {self.buffer!r}")
        end = self.buffer.index(marker) + len(marker)
        self.buffer = self.buffer[end:]

    def send(self, text):
        os.write(self.master, text.encode() if isinstance(text, str) else text)

    def test_ctrl_c_at_prompt_keeps_shell_alive(self):
        self.send(b"\x03")
        self.read_for(0.1)
        self.send("printf '__ALIVE_IDLE__\\n'\n")
        self.expect(b"__ALIVE_IDLE__")

    def test_ctrl_c_interrupts_foreground_child_and_keeps_shell_alive(self):
        code = "import time; print('__CHILD_READY__', flush=True); time.sleep(30)"
        self.send(f"python3 -c {shlex.quote(code)}\n")
        self.expect(b"__CHILD_READY__")
        self.send(b"\x03")
        self.send("printf '__ALIVE_CHILD__\\n'\n")
        self.expect(b"__ALIVE_CHILD__")

    def test_ctrl_c_interrupts_multistage_pipeline_and_keeps_shell_alive(self):
        self.send("sleep 10 | cat\n")
        self.read_for(0.3)
        self.send(b"\x03")
        self.send("printf '__ALIVE_PIPELINE__\\n'\n")
        self.expect(b"__ALIVE_PIPELINE__")

    def test_ctrl_c_interrupts_builtin_fifo_open_and_restores_output(self):
        os.mkfifo(Path(self.temp.name) / "blocked")
        self.send("pwd > blocked\n")
        self.read_for(0.1)
        self.send(b"\x03")
        self.send("printf '__ALIVE_FIFO__\\n'\n")
        self.expect(b"__ALIVE_FIFO__")

    def test_ctrl_d_at_empty_prompt_exits(self):
        self.send(b"\x04")
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid == self.pid:
                self.reaped = True
                self.assertTrue(os.WIFEXITED(status), status)
                self.assertEqual(os.WEXITSTATUS(status), 0)
                return
            self.read_for(0.05)
        self.fail("Ctrl+D did not exit the shell within 5s")


if __name__ == "__main__":
    unittest.main(verbosity=2)
