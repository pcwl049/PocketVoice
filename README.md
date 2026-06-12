# PocketVoice

PocketVoice 是给 VRChat ChatBox 使用的本地离线语音转文字工具。

它把电脑上的语音输入发送到 Android 手机，由手机完成本地识别，再把文字传回电脑并发送到 VRChat ChatBox。

```text
电脑收音 -> 手机识别 -> 电脑发送到 VRChat ChatBox
```

## 适合谁

- 想在 VRChat ChatBox 里显示自己说话内容的用户。
- 电脑性能有限，希望把语音识别计算放到 Android 手机上运行的用户。

## 当前能力

- Windows 端可选择录音设备。
- PC 端负责收音、语音活动检测、音频分段、USB 转发和 ChatBox 输出。
- Android 端负责接收语音片段，并在本机完成语音识别。
- 当前预览版支持 SenseVoice QNN 后端。

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

## 设备兼容性

当前 QNN HTP 路径只在有限的 Qualcomm Android 测试环境验证。代码会生成如下 QNN HTP 配置：

```text
soc_id: 85
dsp_arch: v79
```

其他 Android 设备可能遇到 QNN 初始化失败、HTP backend 加载失败、性能异常或无法使用 QNN 后端的问题。

## 源码构建

源码构建需要准备本地依赖。大型 SDK、模型和第三方二进制库不放入 Git 仓库。

### PC

需要：

- Visual Studio 2022 或 Build Tools，包含 C++ 工具链。
- Windows SDK。
- WebView2 NuGet 包。
- `third_party/sherpa-onnx-v1.12.39-win-x64-shared-MD-Release/`。
- `models/fireredvad/`。

构建命令：

```powershell
scripts\build_pc.bat
```

如果自动检测 Visual Studio 失败，可以手动设置：

```powershell
$env:VS_DIR = "<Visual Studio installation path>"
$env:MSVC_VER = "14.xx.xxxxx"
$env:WIN_SDK = "<Windows SDK root>"
$env:WIN_SDK_VER = "10.0.xxxxx.0"
scripts\build_pc.bat
```

输出：

```text
build\pc\stt_pc.exe
```

### Android APK

需要：

- Android SDK。
- Android build-tools。
- Android platform。
- Android NDK，推荐 r27c。
- 已构建或准备好的 sherpa-onnx Android arm64 库。
- QNN 构建还需要 Qualcomm QAIRT/QNN SDK。

建议设置：

```powershell
$env:ANDROID_HOME = "<Android SDK path>"
$env:ANDROID_NDK_ROOT = "<Android NDK path>"
$env:QNN_SDK_ROOT = "<QAIRT or QNN SDK path>"
$env:ADB = "$env:ANDROID_HOME\platform-tools\adb.exe"
```

构建 QNN APK：

```powershell
scripts\build_mobile_apk.bat
```

构建 CPU APK：

```powershell
scripts\build_mobile_apk.bat --cpu
```

输出：

```text
build\mobile-apk\app-signed.apk
```

### QNN 相关命令

构建带 QNN 支持的 sherpa-onnx Android 库：

```powershell
scripts\build_qnn_android.bat
```

推送 SenseVoice QNN 模型到手机：

```powershell
scripts\push_sensevoice_qnn_model.bat
```

完整 QNN WAV 验收：

```powershell
scripts\test_qnn_pc_wav_suite.bat
```

## 许可证

PocketVoice 采用 Apache License 2.0。二进制包内包含的第三方组件和模型文件遵循各自许可证，详见 `THIRD_PARTY_NOTICES.txt`。

由 GPT-5.5 构建。
