"""阶段 1a 端到端自动化测试：纯信令转发（不涉及 WebRTC）。

# 这个脚本验证什么
  - 信令服务器能正常起来、监听端口
  - 两个 C++ 客户端能 hello 注册到房间
  - 服务器能把 A 端的 text 消息原样转发给 B 端，反之亦然
  - 两端能正确退出（exit code = 0）

# 工作方式
  用 subprocess 启动 server + A + B 三个进程，通过 stdin/stdout 控制和
  抓取输出。读 A 端的 stdout 看里面有没有 "B 的消息"，反之亦然。

# 注意
  这个脚本"过时"了——阶段 1b 之后，main.cc 把文字消息从信令转发改成了
  通过 WebRTC P2P DataChannel 发，所以这个脚本现在会"假性通过"（因为信令
  服务器仍然会原样转发任何 JSON 消息）。它主要价值是验证信令服务器和 C++
  signaling_client 这层基础设施还能跑。
"""

from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path

# 解出项目根目录（脚本所在目录的父目录）
REPO = Path(__file__).resolve().parent.parent
SERVER_SCRIPT = REPO / "signaling" / "server.py"
CLIENT_EXE = REPO / "build" / "client" / "Release" / "audiosub_client.exe"


def fail(msg: str) -> "None":
    """打印 FAIL 消息并以非零退出码结束。"""
    print(f"FAIL: {msg}")
    sys.exit(1)


def main() -> int:
    # 前置检查：客户端必须先编好
    if not CLIENT_EXE.exists():
        fail(f"client not built: {CLIENT_EXE}")
    if not SERVER_SCRIPT.exists():
        fail(f"server script missing: {SERVER_SCRIPT}")

    # === 第 1 步：拉起信令服务器 ===
    print("[1] starting signaling server ...")
    server = subprocess.Popen(
        [sys.executable, "-u", str(SERVER_SCRIPT), "--port", "8888"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        cwd=REPO,
    )
    # 留 0.7 秒让 server 把 socket bind 起来
    time.sleep(0.7)
    if server.poll() is not None:
        # 已经退出了，肯定有问题
        out = server.stdout.read().decode("utf-8", errors="replace") if server.stdout else ""
        fail(f"server exited early:\n{out}")

    # 工厂函数：启动一个客户端进程，stdin/stdout 都用 pipe 接管
    def start_client(peer_id: str) -> "subprocess.Popen[str]":
        return subprocess.Popen(
            [str(CLIENT_EXE), "--id", peer_id, "--host", "127.0.0.1", "--port", "8888"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,        # 用文本模式，省去编解码
            bufsize=1,        # 行缓冲
            cwd=REPO,
        )

    try:
        # === 第 2-3 步：启动 A、B 客户端 ===
        print("[2] starting client A ...")
        ca = start_client("A")
        time.sleep(0.3)
        print("[3] starting client B ...")
        cb = start_client("B")

        # 等连接稳定 + peer_ready 双向就绪
        time.sleep(1.0)

        # === 第 4-5 步：互发消息 ===
        # 注意：阶段 1b 之后，A/B 都会试图通过 DataChannel 发，
        # 一开始 dc 还没 open，消息会被 main.cc 拒发并打印 "(message dropped)"。
        # 这是预期的，本脚本的"PASS"主要靠后续手动 /quit 不崩溃。
        print("[4] A -> 'hi from A'")
        ca.stdin.write("hi from A\n")
        ca.stdin.flush()

        print("[5] B -> 'hello from B'")
        cb.stdin.write("hello from B\n")
        cb.stdin.flush()

        time.sleep(1.0)

        # === 第 6 步：让两端优雅退出 ===
        print("[6] /quit both")
        ca.stdin.write("/quit\n"); ca.stdin.flush()
        cb.stdin.write("/quit\n"); cb.stdin.flush()

        rc_a = ca.wait(timeout=5)
        rc_b = cb.wait(timeout=5)
        out_a = ca.stdout.read()
        out_b = cb.stdout.read()

        print("\n=== Client A output ===")
        print(out_a)
        print("=== Client B output ===")
        print(out_b)

        # === 第 7 步：断言 ===
        # 这些 assert 是阶段 1a 设计时的预期（文字直接通过信令转发）。
        # 现在阶段 1b 之后文字走 DataChannel，所以这里可能不再都通过，
        # 但脚本本身用作"基础设施冒烟"还能跑（双方至少能看到 peer_ready）。
        ok = True
        if "hello from B" not in out_a:
            ok = False
            print("XX A did not receive B's message")
        if "hi from A" not in out_b:
            ok = False
            print("XX B did not receive A's message")
        if "B is online" not in out_a:
            ok = False
            print("XX A did not see peer_ready for B")
        if "A is online" not in out_b:
            ok = False
            print("XX B did not see peer_ready for A")
        if rc_a != 0 or rc_b != 0:
            ok = False
            print(f"XX client exit codes: A={rc_a}, B={rc_b}")

        if ok:
            print("\nPASS: stage 1a relay works end-to-end")
            return 0
        return 2
    finally:
        # 不管成功失败，一定要把 server 收掉
        if server.poll() is None:
            server.terminate()
            try:
                server.wait(timeout=3)
            except subprocess.TimeoutExpired:
                server.kill()


if __name__ == "__main__":
    sys.exit(main())
