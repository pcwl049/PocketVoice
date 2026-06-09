# PocketVoice

PocketVoice 是一个给 VRChat ChatBox 用的语音转文字工具。

你在电脑上说话，或者让电脑播放一段声音；PocketVoice 把有效语音送到 Android 手机上识别，再把识别出的文字发回电脑，由电脑发送到 VRChat ChatBox。

它的核心目标很简单：

```text
电脑收音 -> 手机识别 -> 电脑发送到 VRChat ChatBox
```

识别计算放在手机本地完成。电脑主要负责收音、分段、连接手机和发送 ChatBox。

## 适合什么场景

- 在 VRChat 里用 ChatBox 显示自己说的话。
- 电脑性能紧张，不想把语音识别计算放在电脑上。
- 没有方便的 PC 麦克风时，用虚拟声卡、系统播放声音或其他输入源测试。
- 想把语音识别、ChatBox 队列和设备状态集中在一个轻量工具里管理。

## 当前能做什么

- 电脑读取 Windows 录音设备，也可以读取默认播放设备的声音。
- 电脑把语音切成短段，通过 USB 转发给手机。
- Android 手机在本地完成语音识别。
- 电脑收到文字后，通过 OSC 发送到 VRChat ChatBox。
- PC 端提供一个轻量控制页面，可以查看状态、选择音频输入、重连手机、暂停队列、清空待发送内容。
- PC 端语音检测模型已经嵌入到 `stt_pc.exe`，运行时不需要再单独携带 `models\silero_vad.onnx`。

## 使用前需要

- 一台 Windows 电脑。
- 一台可以运行当前 Android App 的手机。
- 一根 USB 数据线。
- VRChat，并开启 OSC / ChatBox 相关设置。
- PC 端程序和 Android APK。

当前版本还在预览阶段。正式发布包尚未整理完成，PC 端仍需要把运行库 DLL 和 `config.json` 与程序放在一起。Android 端的模型和识别运行文件也还在打包流程里整理。

## 预览版使用流程

1. 在手机上安装并打开 PocketVoice Android App。
2. 用 USB 连接手机和电脑。
3. 启动 PC 端程序。
4. 打开 PC 控制页面：

```text
http://127.0.0.1:8766/
```

5. 在控制页面选择音频输入。
6. 进入 VRChat，确保 ChatBox 可以接收 OSC 消息。
7. 开始说话，识别文字会按队列发送到 ChatBox。

## 当前限制

- ChatBox 本身更适合分段文本，当前版本按语音片段发送结果。
- USB 转发是当前主要连接方式。
- 模型质量、延迟和内存占用还在继续测试。
- 一键发布包还没完成，当前更接近可运行预览版。
- 更换模型属于后续配置能力，默认模型会优先保留已经通过测试的版本。

## 项目状态

PocketVoice 当前已经跑通：

- Windows 音频输入。
- PC 语音分段。
- USB 传输。
- Android 本地识别。
- PC 接收识别结果。
- VRChat ChatBox 发送链路测试。
- PC 控制页面。

下一步重点会放在发布包、模型配置、真实 VRChat 使用体验和长时间运行稳定性上。
