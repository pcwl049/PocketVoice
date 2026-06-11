# PocketVoice

PocketVoice 是给 VRChat ChatBox 用的语音转文字工具。

它把电脑上的语音输入发送到 Android 手机，由手机完成本地识别，再把文字传回电脑发送到 VRChat ChatBox。

```text
电脑收音 -> 手机识别 -> 电脑发送到 VRChat ChatBox
```

## 适合谁

- 想在 VRChat ChatBox 里显示自己说话内容的用户。
- 电脑性能有限，希望把语音识别计算放到手机上的用户。

## 当前能力

- Windows 端可选择录音设备。
- PC 端负责收音、语音分段、USB 转发和 ChatBox 发送。
- Android 端负责接收语音片段并在本机完成识别。

## 使用前需要

- Windows 电脑。
- Android 手机。
- USB 数据线。
- VRChat，并开启 OSC / ChatBox。
- PocketVoice 预览包。

## 预览版使用流程

1. 解压 PocketVoice 预览包。
2. 在手机上安装 `PocketVoice-Android.apk`。
3. 用 USB 连接手机和电脑，并允许 USB 调试。
4. 打开手机端 PocketVoice，启动服务。
5. 在电脑上运行 `PocketVoice.exe`。
6. 在 PC 控制窗口选择音频输入。
7. 进入 VRChat，确认 OSC / ChatBox 已开启。

## 许可证

PocketVoice 采用 Apache License 2.0 开源。二进制包内包含的第三方组件和模型文件遵循各自许可证，详见 `THIRD_PARTY_NOTICES.txt`。

## 构建说明

由 GPT-5.5 构建。
