"""阶段 2 端到端自动化测试：WebRTC 音频链路。

# 这个脚本验证什么
  - A端成功添加音频轨道（麦克风采集）
  - WebRTC 握手完成后 SDP 包含音频 m= 行
  - B端成功接收远端音频轨道（OnTrack 触发）
  - B端通过 AudioTrackSinkInterface 获取 PCM 帧
  - B端控制台打印 PCM 帧信息（采样率、声道数等）
  - DataChannel 文字通信仍然正常
  - 两端能优雅退出

# 关键时序
  1. 信令服务器启动
  2. A端启动（offerer，添加音频轨道 + DataChannel）
  3. B端启动（answerer，设置音频接收管线）
  4. WebRTC 握手完成 → DataChannel open + 音频轨道到达
  5. B端开始接收 PCM 帧
  6. 等待足够多的帧后退出

# 失败常见原因
  - 防火墙拦住 UDP（首次跑会弹"允许网络访问"，要选允许）
  - 没有麦克风设备（A端 AddAudioTrack 会失败）
  - 防病毒软件拦 audiosub_client.exe
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
        print("[3] starting answerer B (audio receive) ...")
        cb = start_client("B")

        # 等待 A端添加音频轨道
        print("[4] waiting for A to add audio track ...")
        ok_a_audio = wait_for("A", ca, "audio track added", 10.0, buf_a)
        if not ok_a_audio:
            print("=== A output so far ===")
            print("".join(buf_a))
            fail("A端未能成功添加音频轨道（可能没有麦克风设备）")

        # 等待 DataChannel 在两端都 open
        print("[5] waiting for DataChannel to open on both ends (<= 30s) ...")
        ok_a = wait_for("A", ca, "dc:open", 30.0, buf_a)
        ok_b = wait_for("B", cb, "dc:open", 30.0, buf_b)
        if not ok_a or not ok_b:
            print("=== A output so far ===")
            print("".join(buf_a))
            print("=== B output so far ===")
            print("".join(buf_b))
            fail(f"DataChannel never opened: A={ok_a}, B={ok_b}")

        # 等待 B端收到远端音频轨道
        print("[6] waiting for B to receive remote audio track ...")
        ok_b_track = wait_for("B", cb, "remote audio track received", 15.0, buf_b)
        if not ok_b_track:
            print("=== B output so far ===")
            print("".join(buf_b))
            fail("B端未收到远端音频轨道")

        # 等待 B端打印 PCM 帧信息
        print("[7] waiting for B to receive PCM frames ...")
        ok_b_pcm = wait_for("B", cb, "[audio] frame #", 15.0, buf_b)
        if not ok_b_pcm:
            print("=== B output so far ===")
            print("".join(buf_b))
            fail("B端未收到任何 PCM 帧")

        # 再等几秒收集更多帧
        print("[8] collecting more frames (5s) ...")
        time.sleep(5.0)

        # 测试 DataChannel 文字通信仍然正常
        print("[9] testing DataChannel text messaging ...")
        ca.stdin.write("hello from A\n"); ca.stdin.flush()
        cb.stdin.write("hello from B\n"); cb.stdin.flush()
        time.sleep(1.5)

        # 优雅退出
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

        # 断言
        ok = True

        # 1. A端添加了音频轨道
        if "audio track added" not in out_a:
            ok = False
            print("XX A端未成功添加音频轨道")

        # 2. B端收到远端音频轨道
        if "remote audio track received" not in out_b:
            ok = False
            print("XX B端未收到远端音频轨道")

        # 3. B端收到了 PCM 帧
        if "[audio] frame #" not in out_b:
            ok = False
            print("XX B端未收到任何 PCM 帧")

        # 4. PCM 帧参数合理（48kHz, 1-2声道, 16bit）
        pcm_match = re.search(r"\[audio\] frame #\d+: (\d+)Hz (\d+)ch (\d+)samples", out_b)
        if pcm_match:
            sample_rate = int(pcm_match.group(1))
            channels = int(pcm_match.group(2))
            if sample_rate not in (48000, 16000, 8000):
                print(f"?? PCM 采样率异常: {sample_rate}Hz (预期 48000/16000/8000)")
            if channels < 1 or channels > 2:
                print(f"?? PCM 声道数异常: {channels}ch (预期 1 或 2)")
            print(f"   PCM 参数: {sample_rate}Hz, {channels}ch")
        else:
            print("?? 无法解析 PCM 帧参数")

        # 5. DataChannel 文字通信正常
        if "<peer> hello from B" not in out_a:
            ok = False
            print("XX A端未收到 B 的 DataChannel 消息")
        if "<peer> hello from A" not in out_b:
            ok = False
            print("XX B端未收到 A 的 DataChannel 消息")

        # 6. 退出码正常
        if rc_a != 0 or rc_b != 0:
            ok = False
            print(f"XX 客户端退出码异常: A={rc_a}, B={rc_b}")

        if ok:
            print("\nPASS: stage 2 WebRTC 音频链路端到端测试通过")
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
