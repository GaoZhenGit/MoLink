"""Level 2: USB device tests. Each test manages its own daemon lifecycle."""
import pytest
import os
import tempfile
import re
from test_utils import run_molink, wait_for_status, random_content, random_name, md5_file

REMOTE_DIR = "/sdcard/tmp"


def _start_daemon():
    """Ensure a fresh daemon is running. Returns True if connected."""
    run_molink("stop")
    run_molink("start")
    ok, status = wait_for_status("daemon=running", timeout=10)
    return ok and "state=connected" in status


@pytest.mark.level2
class TestDevice:
    """USB 设备相关功能测试（需设备已连接已授权）。"""

    # ---- daemon lifecycle ----

    def test_daemon_start_stop(self):
        """启动 daemon：验证 status(connected)、devices(含设备)、shell(可用)。
        然后停止：验证 status(stopped)。一次启停覆盖多个检查。"""
        assert _start_daemon(), "daemon failed to start"

        # status
        _, status, _ = run_molink("status")
        assert "state=connected" in status
        assert "serial=" in status

        # devices
        _, dev_out, _ = run_molink("devices")
        assert "yes" in dev_out, f"no authorized device:\n{dev_out}"

        # shell（验证设备通信可用）
        _, shell_out, _ = run_molink("shell", "echo ok")
        assert "ok" in shell_out

        # ls（验证文件系统可访问）
        rc, ls_out, _ = run_molink("ls", "/sdcard/")
        assert rc == 0 and len(ls_out) > 0, "ls failed or empty"

        # stop
        run_molink("stop")
        _, stop_status, _ = run_molink("status")
        assert "daemon=stopped" in stop_status

    # ---- file transfer ----

    def test_file_push_pull_md5(self):
        """push 随机文件 → pull 回来 → md5 一致 → del 清理远端。"""
        assert _start_daemon(), "daemon failed to start"

        name = random_name()
        local_src = os.path.join(tempfile.gettempdir(), f"ml_src_{name}.bin")
        local_dst = os.path.join(tempfile.gettempdir(), f"ml_dst_{name}.bin")
        remote = f"{REMOTE_DIR}/ml_{name}.bin"

        content = random_content(4096)
        with open(local_src, "wb") as f:
            f.write(content)
        src_md5 = md5_file(local_src)

        try:
            assert run_molink("push", local_src, remote)[0] == 0, "push failed"
            assert run_molink("pull", remote, local_dst)[0] == 0, "pull failed"
            assert md5_file(local_dst) == src_md5, "md5 mismatch"
            run_molink("del", remote)
        finally:
            for f in [local_src, local_dst]:
                if os.path.exists(f):
                    os.remove(f)

        run_molink("stop")

    def test_apush_apull(self):
        """apush 打包目录上传 → apull 下载并解压（用 pull 验证 zip 存在）。
        覆盖自动压缩、base64 命名、远程清理。"""
        assert _start_daemon(), "daemon failed to start"

        name = random_name()
        tmp_dir = os.path.join(tempfile.gettempdir(), f"ml_ap_{name}")
        os.makedirs(os.path.join(tmp_dir, "sub"), exist_ok=True)
        with open(os.path.join(tmp_dir, "a.txt"), "wb") as f:
            f.write(random_content(512))
        with open(os.path.join(tmp_dir, "sub", "b.txt"), "wb") as f:
            f.write(random_content(512))

        try:
            # apush
            assert run_molink("apush", tmp_dir, "--rdir", REMOTE_DIR)[0] == 0

            # 从 ls 中找到 b64_ 文件名，pull 下来验证存在且非空
            _, ls_out, _ = run_molink("ls", REMOTE_DIR)
            m = re.search(r"b64_([A-Za-z0-9+/=]+)", ls_out)
            assert m, f"b64_ file not found:\n{ls_out}"

            remote_zip = f"{REMOTE_DIR}/b64_{m.group(1)}"
            local_zip = os.path.join(tempfile.gettempdir(), f"ml_apull_{name}.zip")
            assert run_molink("pull", remote_zip, local_zip)[0] == 0
            assert os.path.getsize(local_zip) > 100, "zip too small"

            run_molink("del", remote_zip)
            os.remove(local_zip)
        finally:
            import shutil
            if os.path.exists(tmp_dir):
                shutil.rmtree(tmp_dir)

        run_molink("stop")

    # ---- port forward ----

    def test_forward(self):
        """forward 启动 → status 含 forwarding=1080→1081 → daemon 仍可用（ls 正常）。"""
        assert _start_daemon(), "daemon failed to start"

        assert run_molink("forward", "-p", "1080", "-r", "1081")[0] == 0
        _, status, _ = run_molink("status")
        assert "forwarding=1080->1081" in status

        # daemon 其他功能不受影响
        assert run_molink("ls", "/sdcard/")[0] == 0

        run_molink("stop")
