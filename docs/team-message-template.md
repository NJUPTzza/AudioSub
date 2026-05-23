# AudioSub 组员接入消息模板

下面这段已经按当前仓库和 SDK 地址填好了，直接复制发给组员即可。

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
git clone https://github.com/NJUPTzza/AudioSub.git
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
git clone https://github.com/NJUPTzza/AudioSub.git
cd AudioSub

3. 一键准备并编译：
.\scripts\bootstrap.ps1 -Url "https://github.com/NJUPTzza/AudioSub/releases/download/webrtc-sdk-v1.0.0/webrtc-sdk-win-x64-release-771e6489c9.zip"

4. 跑 demo（开 3 个终端）：
终端1：python signaling\server.py
终端2：.\build\client\Release\audiosub_client.exe --id A
终端3：.\build\client\Release\audiosub_client.exe --id B

如果 A/B 两边看到 dc:open，就表示链路通了。任意一端输入文字，另一端应该能看到 <peer> 消息。

如果遇到问题，先看：
docs\new-collaborator-setup.md
```

## 当前已填入的信息

- GitHub 仓库地址：`https://github.com/NJUPTzza/AudioSub`
- SDK 下载地址：`https://github.com/NJUPTzza/AudioSub/releases/download/webrtc-sdk-v1.0.0/webrtc-sdk-win-x64-release-771e6489c9.zip`
