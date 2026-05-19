"""Level 3: end-to-end proxy tests. Requires worker running on device + network.

Protocol coverage:
  - SOCKS5:  curl --socks5            (client resolves DNS)
  - SOCKS5h: curl --socks5-hostname   (proxy resolves DNS)
"""
import pytest
import subprocess
import os
from test_utils import run_molink, wait_for_status

SOCKS5_AUTH = ["--proxy-user", "socks5:password123"]
TEST_URLS = [
    "http://httpbin.org/ip",
    "http://httpbin.org/get",
    "http://httpbin.org/headers",
]
SINGLE_URL = TEST_URLS[0]


def _curl(desc, args, timeout=10):
    """Run curl with description, print command + result, return HTTP status code."""
    p = subprocess.run(
        ["curl", "--max-time", str(timeout), "-s", "-o", os.devnull, "-w", "%{http_code}"] + args,
        capture_output=True, text=True, timeout=timeout + 5
    )
    code = p.stdout.strip()
    # print compactly: curl <url> -> HTTP <code>
    url = args[-1] if args else "?"
    print(f"\n  $ curl {desc} {url}\n    -> HTTP {code}")
    return code


def _ensure_proxy():
    """Start daemon + forward. Return True if proxy is responding."""
    run_molink("stop")
    run_molink("start")
    ok, status = wait_for_status("daemon=running", timeout=10)
    if not ok:
        return False
    run_molink("forward", "-p", "1080", "-r", "1081")
    _, status, _ = run_molink("status")
    if "forwarding=1080" not in status:
        return False
    # Verify proxy works
    return _curl("(pre-check)", ["--socks5", "127.0.0.1:1080"] + SOCKS5_AUTH + [SINGLE_URL]) == "200"


@pytest.mark.level3
class TestE2E:
    """端到端代理测试：覆盖 SOCKS5（客户端 DNS）和 SOCKS5h（代理端 DNS）。"""

    _ready = False

    @classmethod
    def setup_class(cls):
        cls._ready = _ensure_proxy()

    @classmethod
    def teardown_class(cls):
        run_molink("stop")

    def test_socks5(self):
        """SOCKS5 代理：客户端解析 DNS，代理仅转发 TCP。
        验证多个不同 URL 均返回 200。"""
        assert self._ready, "proxy not available"
        for url in TEST_URLS:
            code = _curl("SOCKS5", ["--socks5", "127.0.0.1:1080"] + SOCKS5_AUTH + [url])
            assert code == "200", f"SOCKS5 {url} -> HTTP {code}"

    def test_socks5h(self):
        """SOCKS5h 代理：代理端解析 DNS（--socks5-hostname）。
        验证代理能正确解析域名并连接目标。"""
        assert self._ready
        codes = []
        for url in TEST_URLS:
            code = _curl("SOCKS5h", ["--socks5-hostname", "127.0.0.1:1080"] + SOCKS5_AUTH + [url])
            codes.append(code)
        assert all(c == "200" for c in codes), f"SOCKS5h codes: {codes}"

    def test_reconnect(self):
        """断连恢复：stop daemon 后代理不可用 → start + forward 后恢复。
        验证 SOCKS5 代理在新 daemon 实例下正常工作。"""
        assert self._ready

        # Stop and verify proxy is down
        run_molink("stop")
        code_down = _curl("SOCKS5(down)", ["--socks5", "127.0.0.1:1080"] + SOCKS5_AUTH + [SINGLE_URL])
        assert code_down != "200", f"proxy should be down, got {code_down}"

        # Restart and verify recovery
        assert _ensure_proxy(), "failed to restart proxy"
        code_up = _curl("SOCKS5(up)", ["--socks5", "127.0.0.1:1080"] + SOCKS5_AUTH + [SINGLE_URL])
        assert code_up == "200", f"proxy not recovered, got {code_up}"
