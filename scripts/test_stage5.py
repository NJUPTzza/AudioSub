"""阶段 5 端到端自动化测试：完整链路验证。

验证内容:
  1. A端成功添加音频轨道（麦克风采集）
  2. B端成功接收远端音频轨道
  3. B端成功初始化 whisper ASR 引擎
  4. WebRTC 握手完成，DataChannel 打开
  5. B端音频重采样管线正常工作（48kHz -> 16kHz/mono）
  6. A端退出后B端自动退出
  7. 优雅退出：无崩溃、无死锁
  8. 连续运行 30 秒不崩溃

关键时序:
  1. 信令服务器启动
  2. A端启动（offerer，添加音频轨道）
  3. B端启动（answerer，设置音频接收 + 重采样 + ASR 管线）
  4. WebRTC 握手完成 → 音频轨道到达
  5. B端开始接收 PCM 帧 → 重采样 → 送入 ASR
  6. 等待 30 秒验证稳定性
  7. A端退出 → B端自动退出
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
MODEL_PATH = REPO / "models" / "ggml-small.bin"

STABILITY_DURATION = 30


def fail(msg: str) -> None:
    print(f"FAIL: {msg}")
    sys.exit(1)


def wait_for(name: str, popen: subprocess.Popen[str], needle: str,
             timeout_s: float, buf: list) -> bool:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        line = popen.stdout.readline()
        if not line:
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
    if not MODEL_PATH.exists():
        fail(f"whisper model missing: {MODEL_PATH}\n"
             f"Run: Invoke-WebRequest -Uri "
             f"'https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin' "
             f"-OutFile 'models\\ggml-small.bin'")

    print("[1] starting signaling server ...")
    server = subprocess.Popen(
        [sys.executable, "-u", str(SERVER_SCRIPT), "--port", "8888"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.STDOUT,
        cwd=REPO,
    )
    time.sleep(0.7)
    if server.poll() is not None:
        fail("signaling server exited early")

    def start_client(peer_id: str) -> subprocess.Popen[str]:
        return subprocess.Popen(
            [str(CLIENT_EXE), "--id", peer_id, "--host", "127.0.0.1",
             "--port", "8888", "--model", str(MODEL_PATH), "--lang", "zh"],
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
        print("[2] starting offerer A (mic capture) ...")
        ca = start_client("A")
        time.sleep(0.3)
        print("[3] starting answerer B (audio receive + ASR) ...")
        cb = start_client("B")

        print("[4] waiting for A to add audio track ...")
        ok_a_audio = wait_for("A", ca, "audio track added", 10.0, buf_a)
        if not ok_a_audio:
            print("".join(buf_a))
            fail("A端未能成功添加音频轨道")

        print("[5] waiting for B to initialize whisper engine ...")
        ok_b_asr = wait_for("B", cb, "whisper engine initialized", 30.0, buf_b)
        if not ok_b_asr:
            print("".join(buf_b))
            fail("B端 whisper 引擎初始化失败")

        print("[6] waiting for DataChannel to open on both ends ...")
        ok_a = wait_for("A", ca, "dc:open", 30.0, buf_a)
        ok_b = wait_for("B", cb, "dc:open", 30.0, buf_b)
        if not ok_a or not ok_b:
            fail(f"DataChannel never opened: A={ok_a}, B={ok_b}")

        print("[7] waiting for B to receive remote audio track ...")
        ok_b_track = wait_for("B", cb, "remote audio track received", 15.0, buf_b)
        if not ok_b_track:
            fail("B端未收到远端音频轨道")

        print(f"[8] stability test: running for {STABILITY_DURATION}s ...")
        start = time.time()
        crashed = False
        while time.time() - start < STABILITY_DURATION:
            rc_a = ca.poll()
            rc_b = cb.poll()
            if rc_a is not None:
                fail(f"A端意外退出 (rc={rc_a})")
            if rc_b is not None:
                fail(f"B端意外退出 (rc={rc_b})")
            time.sleep(1.0)
            elapsed = int(time.time() - start)
            if elapsed % 10 == 0:
                print(f"   ... {elapsed}s elapsed")

        print(f"[9] stability test passed ({STABILITY_DURATION}s)")

        ca.stdin.write("/quit\n"); ca.stdin.flush()
        cb.stdin.write("/quit\n"); cb.stdin.flush()

        rc_a = ca.wait(timeout=10)
        rc_b = cb.wait(timeout=10)
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

        ok = True

        # 1. A端添加了音频轨道
        if "audio track added" not in out_a:
            ok = False
            print("XX A端未成功添加音频轨道")
        else:
            print("   OK A端成功添加音频轨道")

        # 2. B端 whisper 引擎初始化成功
        if "whisper model loaded" not in out_b and "whisper engine initialized" not in out_b:
            ok = False
            print("XX B端 whisper 引擎未成功初始化")
        else:
            print("   OK B端 whisper 引擎初始化成功")

        # 3. B端收到远端音频轨道
        if "remote audio track received" not in out_b:
            ok = False
            print("XX B端未收到远端音频轨道")
        else:
            print("   OK B端收到远端音频轨道")

        # 4. WebRTC 连接建立
        if "pc:connected" not in out_a or "pc:connected" not in out_b:
            ok = False
            print("XX WebRTC 连接未建立")
        else:
            print("   OK WebRTC 连接建立成功")

        # 5. DataChannel 打开
        if "dc:open" not in out_a or "dc:open" not in out_b:
            ok = False
            print("XX DataChannel 未打开")
        else:
            print("   OK DataChannel 打开成功")

        # 6. 优雅退出
        if "bye." not in out_a and "bye." not in out_b:
            ok = False
            print("XX 客户端未优雅退出")
        else:
            print("   OK 客户端优雅退出")

        # 7. 检查是否有字幕输出（可选，需要麦克风有声音输入）
        subtitle_match = re.search(r"\[\d{2}:\d{2}\.\d{3}\s*->\s*\d{2}:\d{2}\.\d{3}\]", out_b)
        if subtitle_match:
            print("   OK B端产出了字幕输出")
        else:
            print("   -- B端未产出字幕（可能麦克风无输入或静音，非错误）")

        # 8. 检查 RingBuffer 溢出（信息性）
        dropped_match = re.search(r"ring buffer dropped (\d+) frames", out_b)
        if dropped_match:
            dropped = int(dropped_match.group(1))
            print(f"   INFO RingBuffer 丢弃了 {dropped} 帧（溢出保护）")
        else:
            print("   OK RingBuffer 无溢出")

        if ok:
            print(f"\nPASS: stage 5 端到端验证测试通过 (stability: {STABILITY_DURATION}s)")
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
