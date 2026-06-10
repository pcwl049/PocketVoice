const assert = require("assert");
const fs = require("fs");
const path = require("path");

const root = path.resolve(__dirname, "..");
const cmake = fs.readFileSync(
  path.join(root, "src/mobile/app/src/main/cpp/CMakeLists.txt"),
  "utf8",
);
const buildScript = fs.readFileSync(path.join(root, "scripts/build_mobile_apk.bat"), "utf8");
const pushQnnModelScript = fs.readFileSync(
  path.join(root, "scripts/push_sensevoice_qnn_model.bat"),
  "utf8",
);
const engineHeader = fs.readFileSync(
  path.join(root, "src/mobile/app/src/main/cpp/stt_engine.h"),
  "utf8",
);
const engineSource = fs.readFileSync(
  path.join(root, "src/mobile/app/src/main/cpp/stt_engine.cpp"),
  "utf8",
);
const jniBridge = fs.readFileSync(
  path.join(root, "src/mobile/app/src/main/cpp/jni_bridge.cpp"),
  "utf8",
);
const networkSource = fs.readFileSync(
  path.join(root, "src/mobile/app/src/main/cpp/network.cpp"),
  "utf8",
);
const mainActivity = fs.readFileSync(
  path.join(root, "src/mobile/app/src/main/java/com/stt/mobile/MainActivity.java"),
  "utf8",
);
const foregroundServicePath = path.join(root, "src/mobile/app/src/main/java/com/stt/mobile/SttForegroundService.java");
const foregroundService = fs.existsSync(foregroundServicePath) ? fs.readFileSync(foregroundServicePath, "utf8") : "";
const manifest = fs.readFileSync(
  path.join(root, "src/mobile/app/src/main/AndroidManifest.xml"),
  "utf8",
);
const androidStyles = fs.readFileSync(
  path.join(root, "src/mobile/app/src/main/res/values/styles.xml"),
  "utf8",
);
const uiIndexPath = path.join(root, "src/mobile/app/src/main/assets/ui/index.html");
const uiStylesPath = path.join(root, "src/mobile/app/src/main/assets/ui/styles.css");
const uiAppPath = path.join(root, "src/mobile/app/src/main/assets/ui/app.js");

assert(
  cmake.includes("STT_USE_QNN") && cmake.includes("${ANDROID_ABI}-qnn"),
  "Android CMake should support STT_USE_QNN and the arm64-v8a-qnn lib directory",
);
assert(
  cmake.includes("audio_job_queue.cpp") && cmake.includes("audio_job_queue.h"),
  "Android CMake should build the native audio job queue",
);
assert(
  buildScript.includes('set "USE_QNN=1"') &&
    buildScript.includes("--cpu") &&
    buildScript.includes("-DSTT_USE_QNN=ON") &&
    buildScript.includes("%BUILD_DIR%\\lib\\arm64-v8a\\libQnn*.so") &&
    buildScript.includes("models\\sensevoice\\libmodel.so") &&
    buildScript.includes("lib/arm64-v8a/libmodel.so") &&
    buildScript.includes("assets/ui/index.html") &&
    buildScript.includes("SttForegroundService.java") &&
    buildScript.includes("*.flat") &&
    buildScript.includes("libc++_shared.so") &&
    buildScript.includes("STT_SKIP_QNN_LIBMODEL_REPAIR"),
  "APK build script should default to QNN, allow CPU fallback, and package QNN runtime/model libraries",
);
assert(
  engineHeader.includes("SenseVoiceQnn"),
  "BackendType should include SenseVoiceQnn",
);
assert(
  engineSource.includes("sensevoice_qnn") &&
    engineSource.includes("SherpaOnnxCreateOfflineRecognizer") &&
    engineSource.includes("SherpaOnnxAcceptWaveformOffline") &&
    engineSource.includes("qnn_backend_lib") &&
    engineSource.includes("qnn_context_binary") &&
    engineSource.includes("qnn_system_lib"),
  "STT engine should implement the SenseVoice QNN offline recognizer path",
);
assert(
  mainActivity.includes("\"sensevoice\"") &&
    mainActivity.includes("model.bin") &&
    mainActivity.includes("libmodel.so") &&
    mainActivity.includes("getApplicationInfo().nativeLibraryDir"),
  "MainActivity should prefer the SenseVoice QNN model directory when a context or model library is present",
);
assert(
    mainActivity.includes("new WebView") &&
    mainActivity.includes("APP_BACKGROUND") &&
    mainActivity.includes("addJavascriptInterface") &&
    mainActivity.includes("@JavascriptInterface") &&
    mainActivity.includes("file:///android_asset/ui/index.html") &&
    mainActivity.includes("SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN") &&
    mainActivity.includes("SYSTEM_UI_FLAG_IMMERSIVE_STICKY") &&
    mainActivity.includes("setDecorFitsSystemWindows(false)") &&
    mainActivity.includes("setNavigationBarContrastEnforced(false)") &&
    mainActivity.includes("LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES") &&
    mainActivity.includes("onWindowFocusChanged") &&
    mainActivity.includes("setVerticalScrollBarEnabled(false)") &&
    mainActivity.includes("MATCH_PARENT") &&
    mainActivity.includes("setUseWideViewPort(false)") &&
    mainActivity.includes("setSupportZoom(false)") &&
    mainActivity.includes("setBuiltInZoomControls(false)") &&
    mainActivity.includes("setDisplayZoomControls(false)") &&
    mainActivity.includes("getSnapshot"),
  "MainActivity should load the local WebView UI, prevent page zoom, and expose the STT JavaScript bridge",
);
assert(
  mainActivity.includes("Manifest.permission.POST_NOTIFICATIONS") &&
    mainActivity.includes("requestPermissions") &&
    mainActivity.includes("SttForegroundService.start") &&
    mainActivity.includes("SttForegroundService.stop") &&
    mainActivity.includes("SttForegroundService.isRunning") &&
    mainActivity.includes("started = SttForegroundService.isRunning()") &&
    !mainActivity.includes("if (started) SttForegroundService.stop(this)") &&
    !mainActivity.includes("if (started) nativeStop()"),
  "MainActivity should request notification permission and delegate long-running STT work to the foreground service without stopping it on Activity destruction",
);
assert(
  fs.existsSync(foregroundServicePath) &&
    foregroundService.includes("extends Service") &&
    foregroundService.includes("startForeground") &&
    foregroundService.includes("FOREGROUND_SERVICE_TYPE_DATA_SYNC") &&
    foregroundService.includes("NotificationChannel") &&
    foregroundService.includes("PowerManager.PARTIAL_WAKE_LOCK") &&
    foregroundService.includes("setReferenceCounted(false)") &&
    foregroundService.includes("wakeLock.release()") &&
    foregroundService.includes("START_STICKY") &&
    foregroundService.includes("MainActivity.nativeInit") &&
    foregroundService.includes("MainActivity.nativeStart") &&
    foregroundService.includes("MainActivity.nativeStop"),
  "SttForegroundService should keep the native STT server alive with a foreground notification and partial wake lock",
);
assert(
  mainActivity.includes("lastLoggedStatus") &&
    mainActivity.includes("Status: ") &&
    mainActivity.includes("private volatile boolean stopping") &&
    mainActivity.includes('new Thread(() ->') &&
    mainActivity.includes('"stt-stop"') &&
    mainActivity.includes('addLog("Status: stopped")') &&
    mainActivity.includes('lastLoggedStatus = "stopped"') &&
    !mainActivity.includes("addLog(\"[status] \" + status)") &&
    !mainActivity.includes("requires:"),
  "MainActivity should log status transitions, stop native work off the UI thread, and avoid noisy setup/help rows",
);
assert(
  manifest.includes('android:theme="@style/AppTheme"') &&
    manifest.includes('android:versionCode="1"') &&
    manifest.includes('android:minSdkVersion="24"') &&
    manifest.includes('android:targetSdkVersion="34"') &&
    manifest.includes('android.permission.FOREGROUND_SERVICE') &&
    manifest.includes('android.permission.FOREGROUND_SERVICE_DATA_SYNC') &&
    manifest.includes('android.permission.POST_NOTIFICATIONS') &&
    manifest.includes('android.permission.WAKE_LOCK') &&
    manifest.includes('android:name=".SttForegroundService"') &&
    manifest.includes('android:foregroundServiceType="dataSync"') &&
    androidStyles.includes("windowNoTitle") &&
    androidStyles.includes("windowFullscreen") &&
    androidStyles.includes("NoActionBar"),
  "Android WebView activity should use a no-title fullscreen theme",
);
assert(fs.existsSync(uiIndexPath), "WebView UI should include index.html asset");
assert(fs.existsSync(uiStylesPath), "WebView UI should include styles.css asset");
assert(fs.existsSync(uiAppPath), "WebView UI should include app.js asset");
const uiIndex = fs.existsSync(uiIndexPath) ? fs.readFileSync(uiIndexPath, "utf8") : "";
const uiStyles = fs.existsSync(uiStylesPath) ? fs.readFileSync(uiStylesPath, "utf8") : "";
const uiApp = fs.existsSync(uiAppPath) ? fs.readFileSync(uiAppPath, "utf8") : "";
assert(
  uiIndex.includes("VRChat ChatBox STT") &&
    uiIndex.includes("Voice Bridge") &&
    uiIndex.includes("data-cache-toggle") &&
    uiIndex.includes("user-scalable=no") &&
    uiIndex.includes("maximum-scale=1") &&
    uiIndex.includes("styles.css") &&
    uiIndex.includes("app.js"),
  "WebView HTML should use the approved Voice Bridge copy, prevent viewport zoom, and load local CSS/JS",
);
assert(
  uiStyles.includes("prefers-reduced-motion") &&
    uiStyles.includes("room-light-a") &&
    uiStyles.includes("background: var(--bg)") &&
    uiStyles.includes("width: 100%") &&
    uiStyles.includes("overflow-wrap: anywhere") &&
    uiStyles.includes("flex: 0 0 auto") &&
    uiStyles.includes(".log-panel") &&
    uiStyles.includes("flex: 1 1 166px") &&
    !uiStyles.includes("margin-top: auto") &&
    !uiStyles.includes("box-shadow: 0 0"),
  "WebView CSS should include low-motion ambient background and avoid auto-pushed dead space",
);
assert(
  uiApp.includes("window.STT") &&
    uiApp.includes("getSnapshot") &&
    uiApp.includes("setCacheEnabled") &&
    uiApp.includes("setInterval") &&
    uiApp.includes("500") &&
    uiApp.includes('status === "stopped"') &&
    uiApp.includes("\\u7b49\\u5f85"),
  "WebView JS should poll the STT bridge snapshot for status updates",
);
assert(
  jniBridge.includes("ADSP_LIBRARY_PATH") &&
    jniBridge.includes("nativeSetRecognitionCacheEnabled") &&
    jniBridge.includes("setenv"),
  "Native init should set ADSP_LIBRARY_PATH for QNN skel lookup",
);
assert(
  jniBridge.includes("audio_job_queue.h") &&
    jniBridge.includes("AudioJobQueue") &&
    jniBridge.includes("recognizerWorker") &&
    jniBridge.includes("tryPush") &&
    jniBridge.includes("waitPop") &&
    jniBridge.includes("Audio queue full"),
  "JNI bridge should queue audio on the network thread and recognize on a worker thread",
);
assert(
  jniBridge.includes("audio.segment_id") &&
    networkSource.includes("FLAG_HAS_SEGMENT_ID") &&
    networkSource.includes("audio.segment_id") &&
    networkSource.includes("segmentId"),
  "Android native transport should parse audio segment ids and return them with text responses",
);
assert(
  networkSource.includes("shutdown(m_impl->serverSocket, SHUT_RDWR)") &&
    networkSource.includes("shutdown(clientSocket, SHUT_RDWR)") &&
    networkSource.includes("closeClientIfCurrent") &&
    networkSource.includes("if (m_clientThread.joinable())"),
  "NetworkServer should shutdown sockets before joining worker threads and close client sockets through the shared lock",
);
assert(
  pushQnnModelScript.includes("model.bin or") &&
    pushQnnModelScript.includes("libmodel.so") &&
    pushQnnModelScript.includes("rm -f") &&
    pushQnnModelScript.includes("DEVICE_DIR%/model.bin") &&
    pushQnnModelScript.includes("DEVICE_DIR%/libmodel.so") &&
    pushQnnModelScript.includes("STT_SENSEVOICE_QNN_LIBMODEL") &&
    pushQnnModelScript.includes("STT_SENSEVOICE_QNN_LIBMODEL_FIRST"),
  "SenseVoice QNN push script should support libmodel.so first-run context generation",
);

const qnnSuite = fs.readFileSync(path.join(root, "scripts/test_qnn_pc_wav_suite.bat"), "utf8");
assert(
  qnnSuite.includes("sensevoice-act16-fixed-prompt-expanded-preserve-layout-restrict") &&
    qnnSuite.includes("STT_SKIP_QNN_LIBMODEL_REPAIR=1") &&
    qnnSuite.includes("STT_SENSEVOICE_QNN_LIBMODEL_FIRST=1"),
  "QNN PC WAV suite should use the verified act16 fixed-prompt model library",
);

console.log("mobile_qnn_integration tests passed");
