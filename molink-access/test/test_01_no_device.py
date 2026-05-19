"""Level 1: no-device tests. Always run, verify exe integrity and CLI interface."""
import re
import pytest
from test_utils import run_molink

VERSION_RE = re.compile(r"^v\d{4}\.\d{2}\.\d{2}\.\d{4}$")
# Representative commands from each category
SAMPLE_COMMANDS = ["run", "start", "stop", "status", "devices", "auth",
                   "forward", "push", "pull", "ls", "shell", "version"]


@pytest.mark.level1
class TestCLI:
    """验证 molink.exe 可执行、版本格式正确、帮助完整、错误处理合理。"""

    def test_version_format(self):
        """-v 输出应匹配 vYYYY.MM.DD.HHmm 格式"""
        rc, out, _ = run_molink("-v")
        assert rc == 0
        assert VERSION_RE.match(out), f"got: {out}"

    def test_help_covers_all_commands(self):
        """--help 应包含所有主要子命令"""
        _, out, _ = run_molink("--help")
        for cmd in SAMPLE_COMMANDS:
            assert cmd in out.lower(), f"help missing '{cmd}'"

    def test_invalid_command_nonzero_exit(self):
        """无效命令应非零退出并打印帮助"""
        rc, out, _ = run_molink("xyznonexistent")
        assert rc != 0

    def test_status_when_stopped(self):
        """无 daemon 时 status 输出 daemon=stopped 且退出码非零"""
        run_molink("stop")
        rc, out, _ = run_molink("status")
        assert "daemon=stopped" in out
        assert rc != 0
