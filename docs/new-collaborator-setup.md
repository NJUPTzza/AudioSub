# AudioSub 新同学入组说明

## 1. 你需要安装什么

### 必需

- Windows 10/11 x64
- Git
- Visual Studio 2022 或 Build Tools 2022
- 安装 `Desktop development with C++`

### 推荐

- Python 3.8+

说明：

- 运行信令服务器和自动化测试需要 `Python`
- 不需要安装 `depot_tools`
- 不需要拉取 `webrtc/src`
- 不需要自己编译 `WebRTC`

## 2. 你会拿到什么

你只需要拿两样东西：

- 项目仓库源码
- 维护者发布的预编译 `WebRTC SDK`

这个 `SDK` 包含：

- `lib/webrtc.lib`
- `include/`
- `VERSION.txt`

## 3. 首次使用步骤

```powershell
git clone https://github.com/<your-user>/AudioSub.git
cd AudioSub
```

### 方式 A：维护者已经配置了默认 SDK 下载地址

直接运行：

```powershell
.\scripts\bootstrap.ps1
```

### 方式 B：维护者给你发了单独的 SDK 下载地址

```powershell
.\scripts\bootstrap.ps1 -Url "https://github.com/<your-user>/AudioSub/releases/download/webrtc-sdk-v1.0.0/webrtc-sdk-win-x64-release.zip"
```

### 方式 C：你已经拿到了本地 SDK 目录

```powershell
.\scripts\bootstrap.ps1 -LocalSdkPath "D:\shared\webrtc-sdk-win-x64-release"
```

## 4. bootstrap.ps1 会帮你做什么

- 检查 `Python`
- 检查 `Visual Studio / Build Tools`
- 检查 `WebRTC SDK`
- 需要时调用 `fetch-webrtc-sdk.ps1`
- 最后调用 `build.ps1` 进行构建

## 5. 如何运行 demo

准备 3 个终端：

### 终端 1

```powershell
python signaling\server.py
```

### 终端 2

```powershell
.\build\client\Release\audiosub_client.exe --id A
```

### 终端 3

```powershell
.\build\client\Release\audiosub_client.exe --id B
```

当两边看到 `dc:open` 后，在任意一端输入文本并回车，另一端应该看到：

```text
<peer> hello
```

## 6. 如何验证当前环境是否正常

### 只重新编译

```powershell
.\scripts\bootstrap.ps1
```

### 跑 WebRTC 端到端测试

```powershell
python .\scripts\test_stage1b.py
```

### 只检查基础信令

```powershell
python .\scripts\test_stage1a.py
```

## 7. 常见问题

### Q1：为什么我不需要拉 WebRTC 源码？

因为团队统一使用维护者打好的 `SDK`，你的机器只负责业务代码编译和运行。

### Q2：为什么我已经装了 VS，但 `cmake` 不在 PATH 也能构建？

`scripts/build.ps1` 会优先尝试自动发现 Visual Studio 自带的 `cmake.exe`。

### Q3：`bootstrap.ps1` 提示找不到 SDK 怎么办？

有三种处理方式：

- 让维护者把下载地址写进 `scripts/fetch-webrtc-sdk.ps1` 里的 `$DefaultReleaseUrl`
- 手动传 `-Url`
- 设置环境变量 `AUDIOSUB_WEBRTC_SDK_URL`

示例：

```powershell
$env:AUDIOSUB_WEBRTC_SDK_URL="https://github.com/<your-user>/<repo>/releases/download/webrtc-sdk-v1.0.0/webrtc-sdk-win-x64-release.zip"
.\scripts\bootstrap.ps1
```

### Q4：`-Clean` 失败，说 build 目录被占用怎么办？

通常是以下进程占用了构建目录：

- Visual Studio
- 文件资源管理器预览
- 正在运行的 `audiosub_client.exe`

关闭相关进程后重试，或者先不带 `-Clean`。

### Q5：我能参与哪些工作，而不需要真的碰 WebRTC？

你可以先参与这些模块：

- 音频缓冲和格式转换
- `ASR` 接入
- 字幕分段输出
- 标注消息协议
- 字幕与标注融合
- 脚本和自动化测试

这些能力可以优先在 `mock/replay` 模式下开发。

## 8. 你真正需要记住的一条命令

如果维护者已经把 SDK 默认地址配置好了，你只需要记住：

```powershell
.\scripts\bootstrap.ps1
```
