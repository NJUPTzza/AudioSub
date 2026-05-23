# AudioSub 组员接入消息模板

下面这段可以直接发到群里，按你的实际仓库地址和 SDK 下载地址替换占位符。

## 版本 A：我已经把默认 SDK 地址写进脚本

```text
大家先按下面步骤准备环境并验证项目：

1. 安装：
- Windows 10/11
- Visual Studio 2022 或 Build Tools 2022
- 勾选 Desktop development with C++
- Python 3.8+
- Git

2. 拉代码：
git clone https://github.com/<your-user>/AudioSub.git
cd AudioSub

3. 一键准备并编译：
.\scripts\bootstrap.ps1

4. 跑 demo（开 3 个终端）：
终端1：python signaling\server.py
终端2：.\build\client\Release\audiosub_client.exe --id A
终端3：.\build\client\Release\audiosub_client.exe --id B

如果 A/B 两边看到 dc:open，就表示链路通了。任意一端输入文字，另一端应该能看到 <peer> 消息。

如果遇到问题，先看：
docs\new-collaborator-setup.md
```

## 版本 B：我还没有把默认 SDK 地址写进脚本

```text
大家先按下面步骤准备环境并验证项目：

1. 安装：
- Windows 10/11
- Visual Studio 2022 或 Build Tools 2022
- 勾选 Desktop development with C++
- Python 3.8+
- Git

2. 拉代码：
git clone https://github.com/<your-user>/AudioSub.git
cd AudioSub

3. 一键准备并编译：
.\scripts\bootstrap.ps1 -Url "https://github.com/<your-user>/<repo>/releases/download/<tag>/webrtc-sdk-win-x64-release-<commit>.zip"

4. 跑 demo（开 3 个终端）：
终端1：python signaling\server.py
终端2：.\build\client\Release\audiosub_client.exe --id A
终端3：.\build\client\Release\audiosub_client.exe --id B

如果 A/B 两边看到 dc:open，就表示链路通了。任意一端输入文字，另一端应该能看到 <peer> 消息。

如果遇到问题，先看：
docs\new-collaborator-setup.md
```

## 建议你发消息前替换的内容

- `<your-user>`
- `<repo>`
- `<tag>`
- `<commit>`
- `GitHub` 仓库实际地址
- `SDK` 实际下载地址
