# AudioSub Signaling Server

最小信令服务器，纯 Python 标准库（asyncio），无第三方依赖。

## 运行

```powershell
python signaling/server.py
# 默认监听 0.0.0.0:8888
# 看详细日志加 -v
python signaling/server.py -v
```

## 协议

TCP 长连接，每行一条 JSON（UTF-8，`\n` 结束）。

### 第一条必须是 hello

```json
{"type":"hello","id":"A"}
```

### 后续消息一律原样转发给另一端

```json
{"type":"text","text":"hello"}
{"type":"offer","sdp":"..."}
{"type":"answer","sdp":"..."}
{"type":"candidate","candidate":"...","sdpMid":"0","sdpMLineIndex":0}
```

### 服务器主动下发

```json
{"type":"peer_ready","peer":"B"}   // 对端已就绪
{"type":"peer_left","peer":"B"}    // 对端断开
```

## 设计原则

- 只做"撮合 + 转发"，不解析业务消息
- 同一个 `id` 重连会踢掉旧连接
- 房间最多两个 peer（A 和 B），第三个进来不会被警告但只会和第一个交流
