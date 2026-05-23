"""AudioSub 最小信令服务器（纯 Python 标准库，无第三方依赖）。

# 什么是"信令服务器"
WebRTC 的两个客户端要建立 P2P 连接前，必须先交换三类信息：
  1. SDP Offer / Answer：描述各自支持的编解码、连接参数
  2. ICE Candidate：自己的网络地址（公网 IP、私网 IP、relay 等）
  3. 协调时机：谁先发起、谁应答、对方是否在线

但 WebRTC **不规定**这三类信息怎么传——你可以用 HTTP、WebSocket、邮件、
鸽子都行。负责把这些信息在 A、B 两端之间"转一手"的服务就是信令服务器。

# 本文件的职责
极简实现：
  - TCP 长连接监听 8888
  - 每行一条 JSON 消息（UTF-8，`\n` 分隔）
  - 客户端 A 发什么消息，原样转给客户端 B；反之亦然
  - 服务器**不解析**业务消息，offer/answer/candidate/text 都是同一段转发逻辑

# 与生产环境的差距（仅用于开发联调）
  - 没有鉴权、没有 TLS、没有重连、没有房间号、没有限流
  - 只支持两个 peer 在同一个"房间"，第三个连上来会和第一个共享 ID

# 用法
    python signaling/server.py                  # 默认监听 0.0.0.0:8888
    python signaling/server.py --port 9000      # 换端口
    python signaling/server.py -v               # 看每条转发的详细日志
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
from typing import Optional


logger = logging.getLogger("signaling")


class Peer:
    """代表一个已连接的客户端。

    持有它的 `id`（比如 "A"/"B"）以及该连接的 StreamWriter（用来回传消息）。
    """

    def __init__(self, peer_id: str, writer: asyncio.StreamWriter) -> None:
        self.id = peer_id
        self.writer = writer

    async def send(self, msg: dict) -> None:
        """把一个字典编码成"一行 JSON + \n"写入这个 peer 的连接。

        发送失败（对端已关、网络断）不抛异常，只打一条 warning：
        信令服务器对单个 peer 的故障要有韧性。
        """
        try:
            line = (json.dumps(msg, ensure_ascii=False) + "\n").encode("utf-8")
            self.writer.write(line)
            await self.writer.drain()
        except Exception as e:
            logger.warning("send to %s failed: %s", self.id, e)


class Room:
    """一个"房间"，里面最多两个 peer。

    本项目目前只用一个全局房间（ROOM 单例）。后续要支持多房间时把这里改成
    `dict[room_id, Room]` 即可，messages 协议里加 `room` 字段。
    """

    def __init__(self) -> None:
        # key=peer_id (字符串)，value=Peer 对象
        self.peers: dict[str, Peer] = {}
        # 操作 self.peers 时用，避免 join/leave 竞态
        self.lock = asyncio.Lock()

    async def join(self, peer: Peer) -> Optional[Peer]:
        """让一个 peer 加入房间，返回房间里已有的另一个 peer（如果有）。

        如果同 ID 重连，会把旧连接强制踢掉（避免幽灵连接占着 ID）。
        """
        async with self.lock:
            if peer.id in self.peers:
                logger.warning("peer %s already in room, kicking old one", peer.id)
                try:
                    self.peers[peer.id].writer.close()
                except Exception:
                    pass
            self.peers[peer.id] = peer
            # 找出房间里 ID 不等于自己的那个 peer（可能没有）
            other = next((p for p in self.peers.values() if p.id != peer.id), None)
            return other

    async def leave(self, peer_id: str) -> Optional[Peer]:
        """把一个 peer 从房间里移除，返回还剩下的 peer（如果有）。"""
        async with self.lock:
            self.peers.pop(peer_id, None)
            return next(iter(self.peers.values()), None)

    def other(self, peer_id: str) -> Optional[Peer]:
        """查找当前 peer 的"对端"。不加锁是因为 dict.values() 在 CPython 下读是安全的。"""
        return next((p for p in self.peers.values() if p.id != peer_id), None)


# 全局唯一的房间。整个服务器进程共享这一个。
ROOM = Room()


async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    """每个 TCP 连接独立跑一份这个协程。

    流程：
      1. 等待客户端发来第一条 hello（包含自己的 id）
      2. 加入房间，通知双方 peer_ready
      3. 进入 readline 循环：每收到一行 JSON，转发给对端
      4. 连接断开时通知对端 peer_left
    """
    addr = writer.get_extra_info("peername")
    logger.info("client connected: %s", addr)

    peer: Optional[Peer] = None
    try:
        # === 第 1 步：握手，等 hello ===
        first_line = await reader.readline()
        if not first_line:
            # 对方刚连上就断开，直接清理
            return
        hello = json.loads(first_line.decode("utf-8"))
        if hello.get("type") != "hello" or "id" not in hello:
            # 协议错误，关掉。这里没回复错误码是因为信令协议非常简单。
            logger.warning("bad hello from %s: %r", addr, hello)
            return

        peer = Peer(str(hello["id"]), writer)
        other = await ROOM.join(peer)
        logger.info("peer %s joined (room size=%d)", peer.id, len(ROOM.peers))

        # === 第 2 步：双向通知 peer_ready ===
        # 这条消息让双方知道"现在可以开始 WebRTC 握手了"。
        # 业务侧约定：A 端收到 peer_ready 就主动 CreateOffer。
        if other is not None:
            await peer.send({"type": "peer_ready", "peer": other.id})
            await other.send({"type": "peer_ready", "peer": peer.id})

        # === 第 3 步：转发循环 ===
        # 每收到一行 JSON 就找对端发过去。服务器**不解析**业务字段（sdp、
        # candidate 等），完全透明转发。
        while True:
            line = await reader.readline()
            if not line:
                # 对端关闭了写端，正常退出
                break
            try:
                msg = json.loads(line.decode("utf-8"))
            except json.JSONDecodeError as e:
                logger.warning("bad json from %s: %s", peer.id, e)
                continue
            target = ROOM.other(peer.id)
            if target is None:
                # 没有对端在线，丢弃消息（业务侧自己决定要不要重试）
                logger.debug("%s sent %s but no peer to relay to", peer.id, msg.get("type"))
                continue
            logger.info("relay %s -> %s: type=%s", peer.id, target.id, msg.get("type"))
            await target.send(msg)

    except (asyncio.IncompleteReadError, ConnectionResetError):
        # 对方非正常断开，常见且不算错误
        pass
    except Exception:
        logger.exception("handler error for %s", addr)
    finally:
        # === 第 4 步：清理 + 通知对端 ===
        if peer is not None:
            remaining = await ROOM.leave(peer.id)
            logger.info("peer %s left", peer.id)
            if remaining is not None:
                # 让另一端知道对方走了，业务侧可以重置 PeerConnection
                await remaining.send({"type": "peer_left", "peer": peer.id})
        try:
            writer.close()
            await writer.wait_closed()
        except Exception:
            pass


async def main_async(host: str, port: int) -> None:
    """asyncio 服务端启动入口。"""
    server = await asyncio.start_server(handle, host, port)
    sockets = server.sockets or []
    bound = ", ".join(str(s.getsockname()) for s in sockets)
    logger.info("signaling server listening on %s", bound)
    # serve_forever 会阻塞直到 Ctrl+C
    async with server:
        await server.serve_forever()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0", help="监听网卡，默认所有")
    parser.add_argument("--port", type=int, default=8888, help="监听端口")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="打印 DEBUG 级日志（包含所有转发细节）")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    try:
        asyncio.run(main_async(args.host, args.port))
    except KeyboardInterrupt:
        logger.info("bye")


if __name__ == "__main__":
    main()
