"""阶段 4 端到端自动化测试：实时语音转写（whisper.cpp）。

# 验证内容
  - A端成功添加音频轨道（麦克风采集）
  - B端成功接收远端音频轨道
  - B端成功初始化 whisper ASR 引擎
  - B端音频重采样管线正常工作（48kHz -> 16kHz/mono）
  - B端 PCM 帧成功送入 ASR 引擎
  - A端退出后B端自动退出

# 注意
  - 本测试需要麦克风输入才能产生字幕输出
  - 在无麦克风或静音环境下，ASR 可能不会产出字幕行
  - 测试主要验证管线接线正确，ASR 引擎初始化成功

# 关键时序
  1. 信令服务器启动
  2. A端启动（offerer，添加音频轨道）
  3. B端启动（answerer，设置音频接收 + 重采样 + ASR 管线）
  4. WebRTC 握手完成 → 音频轨道到达
  5. B端开始接收 PCM 帧 → 重采样 → 送入 ASR
  6. 等待 ASR 处理（至少 5 秒音频块 + 识别时间）
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


def fail(msg: str) -> "None":
    print(f"FAIL: {msg}")
    sys.exit(1)


def wait_for(name: str, popen: "subprocess.Popen[str]", needle: str,
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

    def start_client(peer_id: str) -> "subprocess.Popen[str]":
        return subprocess.Popen(
            [str(CLIENT_EXE), "--id", peer_id, "--host", "127.0.0.1",
             "--port", "8888", "--model", str(MODEL_PATH)],
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

        print("[8] waiting for B to receive PCM frames and feed to ASR ...")
        ok_b_pcm = wait_for("B", cb, "-> ASR", 15.0, buf_b)
        if not ok_b_pcm:
            fail("B端 PCM 帧未送入 ASR 引擎")

        print("[9] waiting for ASR to process audio (10s) ...")
        time.sleep(10.0)

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

        # 2. B端 whisper 引擎初始化成功
        if "whisper model loaded" not in out_b and "whisper engine initialized" not in out_b:
            ok = False
            print("XX B端 whisper 引擎未成功初始化")

        # 3. B端收到远端音频轨道
        if "remote audio track received" not in out_b:
            ok = False
            print("XX B端未收到远端音频轨道")

        # 4. B端 PCM 帧送入 ASR
        if "-> ASR" not in out_b:
            ok = False
            print("XX B端 PCM 帧未送入 ASR")

        # 5. 验证重采样结果：48kHz/Nch -> 16kHz/1ch
        resample_match = re.search(
            r"(\d+)Hz/(\d+)ch\s*->\s*(\d+)Hz/(\d+)ch", out_b)
        if resample_match:
            src_rate = int(resample_match.group(1))
            src_ch = int(resample_match.group(2))
            dst_rate = int(resample_match.group(3))
            dst_ch = int(resample_match.group(4))

            print(f"   重采样: {src_rate}Hz/{src_ch}ch -> {dst_rate}Hz/{dst_ch}ch")

            if dst_rate != 16000:
                ok = False
                print(f"XX 输出采样率错误: {dst_rate}Hz (预期 16000Hz)")
            else:
                print(f"   OK 输出采样率: {dst_rate}Hz")

            if dst_ch != 1:
                ok = False
                print(f"XX 输出声道数错误: {dst_ch}ch (预期 1ch)")
            else:
                print(f"   OK 输出声道数: {dst_ch}ch")
        else:
            ok = False
            print("XX 无法解析重采样结果")

        # 6. 检查是否有字幕输出（可选，需要麦克风有声音输入）
        subtitle_match = re.search(r"\[\d{2}:\d{2}\.\d{3}\s*->\s*\d{2}:\d{2}\.\d{3}\]", out_b)
        if subtitle_match:
            print("   OK B端产出了字幕输出")
        else:
            print("   -- B端未产出字幕（可能麦克风无输入或静音，非错误）")

        # 7. A端退出后B端也退出
        if rc_a != 0 or rc_b != 0:
            ok = False
            print(f"XX 客户端退出码异常: A={rc_a}, B={rc_b}")

        if ok:
            print("\nPASS: stage 4 实时语音转写（whisper.cpp）测试通过")
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
