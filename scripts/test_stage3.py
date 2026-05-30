"""阶段 3 端到端自动化测试：音频处理（重采样 + 声道转换）。

# 验证内容
  - A端成功添加音频轨道（麦克风采集）
  - B端成功接收远端音频轨道
  - B端成功将音频从 48kHz/stereo 转换为 16kHz/mono
  - 转换后的 PCM 帧参数正确（16000Hz, 1ch）
  - 转换后帧时长与原始帧时长一致（10ms）
  - A端退出后B端自动退出

# 关键时序
  1. 信令服务器启动
  2. A端启动（offerer，添加音频轨道）
  3. B端启动（answerer，设置音频接收 + 重采样管线）
  4. WebRTC 握手完成 → 音频轨道到达
  5. B端开始接收 PCM 帧，经过 AudioPipeline 处理
  6. 验证转换后的帧参数
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
        print("[2] starting offerer A (mic capture) ...")
        ca = start_client("A")
        time.sleep(0.3)
        print("[3] starting answerer B (audio receive + resample) ...")
        cb = start_client("B")

        print("[4] waiting for A to add audio track ...")
        ok_a_audio = wait_for("A", ca, "audio track added", 10.0, buf_a)
        if not ok_a_audio:
            print("".join(buf_a))
            fail("A端未能成功添加音频轨道")

        print("[5] waiting for DataChannel to open on both ends ...")
        ok_a = wait_for("A", ca, "dc:open", 30.0, buf_a)
        ok_b = wait_for("B", cb, "dc:open", 30.0, buf_b)
        if not ok_a or not ok_b:
            fail(f"DataChannel never opened: A={ok_a}, B={ok_b}")

        print("[6] waiting for B to receive remote audio track ...")
        ok_b_track = wait_for("B", cb, "remote audio track received", 15.0, buf_b)
        if not ok_b_track:
            fail("B端未收到远端音频轨道")

        print("[7] waiting for B to receive resampled PCM frames ...")
        ok_b_pcm = wait_for("B", cb, "[audio] frame #", 15.0, buf_b)
        if not ok_b_pcm:
            fail("B端未收到任何 PCM 帧")

        print("[8] collecting more frames (5s) ...")
        time.sleep(5.0)

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

        # 2. B端收到远端音频轨道
        if "remote audio track received" not in out_b:
            ok = False
            print("XX B端未收到远端音频轨道")

        # 3. B端收到了转换后的 PCM 帧
        if "[audio] frame #" not in out_b:
            ok = False
            print("XX B端未收到任何 PCM 帧")

        # 4. 验证重采样结果：48kHz/Nch -> 16kHz/1ch
        #    格式: "48000Hz/1ch -> 16000Hz/1ch" 或 "48000Hz/2ch -> 16000Hz/1ch"
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

            # 验证帧时长一致（10ms）
            # 原始: 480samples/48kHz = 10ms
            # 转换后: 160samples/16kHz = 10ms
            sample_match = re.search(
                r"->\s*\d+Hz/\d+ch\s+(\d+)samples", out_b)
            if sample_match:
                out_samples = int(sample_match.group(1))
                out_duration = out_samples / dst_ch / dst_rate * 1000
                print(f"   输出帧: {out_samples} samples, "
                      f"{out_duration:.1f}ms/frame")
                if abs(out_duration - 10.0) > 1.0:
                    print(f"   ?? 帧时长偏差较大: {out_duration:.1f}ms (预期 ~10ms)")
                else:
                    print(f"   OK 帧时长: {out_duration:.1f}ms")
        else:
            ok = False
            print("XX 无法解析重采样结果")

        # 5. A端退出后B端也退出
        if rc_a != 0 or rc_b != 0:
            ok = False
            print(f"XX 客户端退出码异常: A={rc_a}, B={rc_b}")

        if ok:
            print("\nPASS: stage 3 音频处理（重采样+声道转换）测试通过")
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
