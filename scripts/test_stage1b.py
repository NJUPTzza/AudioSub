"""阶段 1b 端到端自动化测试：WebRTC P2P DataChannel。

# 这个脚本验证什么
  - 两个客户端能完成完整的 WebRTC 握手（Offer/Answer/ICE）
  - DataChannel 双向打开（出现 "dc:open" 状态）
  - 文字消息通过 P2P DataChannel 互相到达（出现 "<peer> ..." 行）
  - 两端能优雅退出

# 关键时序
  ICE 协商一般需要 1-5 秒（同机本地最快）。脚本会等待最多 30 秒，等
  "dc:open" 在两端都出现，然后再发文字。

# 失败常见原因
  - 防火墙拦住 UDP（首次跑会弹"允许网络访问"，要选允许）
  - 防病毒软件拦 audiosub_client.exe
  - WebRTC 内部线程没跑起来（看 Initialize() 返回值）
"""

from __future__ import annotations

import re
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SERVER_SCRIPT = REPO / "signaling" / "server.py"
CLIENT_EXE = REPO / "build" / "client" / "Release" / "audiosub_client.exe"


def fail(msg: str) -> "None":
    print(f"FAIL: {msg}")
    sys.exit(1)


def wait_for(name: str, popen: "subprocess.Popen[str]", needle: str,
             timeout_s: float, buf: list) -> bool:
    """从 popen.stdout 里读行，直到出现 needle 子串或超时。

    把读到的每一行追加到 buf 里，方便最后整体打印诊断。
    用 readline 而不是 readlines 是因为我们要边读边检测，不能等到 EOF。
    """
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        line = popen.stdout.readline()
        if not line:
            # 子进程暂时没输出，小睡一下避免空转
            time.sleep(0.05)
            continue
        buf.append(line)
        if needle in line:
            return True
    return False


def main() -> int:
    if not CLIENT_EXE.exists():
        fail(f"client not built: {CLIENT_EXE}")
    if not SERVER_SCRIPT.exists():
        fail(f"server script missing: {SERVER_SCRIPT}")

    # === 第 1 步：起信令服务器 ===
    # 用 -u 让 Python 不缓冲 stdout，方便实时排查问题
    print("[1] starting signaling server ...")
    server = subprocess.Popen(
        [sys.executable, "-u", str(SERVER_SCRIPT), "--port", "8888"],
        stdout=subprocess.DEVNULL,    # 测试时不关心服务器输出
        stderr=subprocess.STDOUT,
        cwd=REPO,
    )
    time.sleep(0.7)
    if server.poll() is not None:
        fail("signaling server exited early")

    def start_client(peer_id: str) -> "subprocess.Popen[str]":
        return subprocess.Popen(
            [str(CLIENT_EXE), "--id", peer_id, "--host", "127.0.0.1", "--port", "8888"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            cwd=REPO,
        )

    buf_a: list[str] = []
    buf_b: list[str] = []

    try:
        # === 第 2-3 步：启动 A（offerer）和 B（answerer）===
        # A 先启动并等 0.3 秒，让它在 B 连进来之前就把 hello 注册到房间。
        # 实际上谁先启动谁后启动都行，main.cc 里 A 收到 peer_ready 后才会
        # CreateOffer，所以顺序不敏感。
        print("[2] starting offerer A ...")
        ca = start_client("A")
        time.sleep(0.3)
        print("[3] starting answerer B ...")
        cb = start_client("B")

        # === 第 4 步：等 DataChannel 在两端都 open ===
        # WebRTC 完整握手包含 ICE 收集、ICE 检查、DTLS 握手，
        # 同机本地通常 1-3 秒搞定，给到 30 秒余量。
        print("[4] waiting for DataChannel to open on both ends (<= 30s) ...")
        ok_a = wait_for("A", ca, "dc:open", 30.0, buf_a)
        ok_b = wait_for("B", cb, "dc:open", 30.0, buf_b)
        if not ok_a or not ok_b:
            # 打印目前的输出帮排查（最常见就是 ICE 卡住）
            print("=== A output so far ===")
            print("".join(buf_a))
            print("=== B output so far ===")
            print("".join(buf_b))
            fail(f"DataChannel never opened: A={ok_a}, B={ok_b}")

        # === 第 5 步：在 P2P 通道上互发文字 ===
        # 到这里 dc 都 open 了，SendMessage 一定会成功。
        print("[5] DataChannel open. Exchanging text ...")
        ca.stdin.write("ping from A\n"); ca.stdin.flush()
        cb.stdin.write("pong from B\n"); cb.stdin.flush()
        # 留时间让消息穿越 P2P 到达
        time.sleep(1.5)

        # 优雅退出
        ca.stdin.write("/quit\n"); ca.stdin.flush()
        cb.stdin.write("/quit\n"); cb.stdin.flush()

        rc_a = ca.wait(timeout=5)
        rc_b = cb.wait(timeout=5)
        # 把剩下的输出抓干净
        try:
            buf_a.append(ca.stdout.read())
            buf_b.append(cb.stdout.read())
        except Exception:
            pass

        out_a = "".join(buf_a)
        out_b = "".join(buf_b)

        print("\n=== Client A output ===")
        print(out_a)
        print("=== Client B output ===")
        print(out_b)

        # === 第 6 步：断言 ===
        ok = True
        # 核心：双方都通过 P2P 通道收到对方消息
        if "<peer> pong from B" not in out_a:
            ok = False
            print("XX A did not receive B's message over DataChannel")
        if "<peer> ping from A" not in out_b:
            ok = False
            print("XX B did not receive A's message over DataChannel")
        # 软警告：检测是否真的走过 P2P 连接状态
        if "pc:connected" not in out_a and "ice:connected" not in out_a and "ice:completed" not in out_a:
            print("?? A never reached a connected ICE state (may still work via host candidates)")
        if rc_a != 0 or rc_b != 0:
            ok = False
            print(f"XX client exit codes: A={rc_a}, B={rc_b}")

        if ok:
            print("\nPASS: stage 1b WebRTC P2P DataChannel works end-to-end")
            return 0
        return 2
    finally:
        if server.poll() is None:
            server.terminate()
            try:
                server.wait(timeout=3)
            except subprocess.TimeoutExpired:
                server.kill()


if __name__ == "__main__":
    sys.exit(main())
