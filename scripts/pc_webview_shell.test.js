const assert = require("assert");
const fs = require("fs");
const path = require("path");

const root = path.resolve(__dirname, "..");
const webviewHeaderPath = path.join(root, "src/pc/pc_webview_window.h");
const webviewSourcePath = path.join(root, "src/pc/pc_webview_window.cpp");

assert(fs.existsSync(webviewHeaderPath), "PC WebView2 shell should have a focused header");
assert(fs.existsSync(webviewSourcePath), "PC WebView2 shell should have a focused implementation");

const header = fs.readFileSync(webviewHeaderPath, "utf8");
const source = fs.readFileSync(webviewSourcePath, "utf8");
const main = fs.readFileSync(path.join(root, "src/pc/main.cpp"), "utf8");
const buildPc = fs.readFileSync(path.join(root, "scripts/build_pc.bat"), "utf8");
const cmake = fs.readFileSync(path.join(root, "src/pc/CMakeLists.txt"), "utf8");
const verifyRelease = fs.readFileSync(path.join(root, "scripts/verify_release.bat"), "utf8");
const statusServerSource = fs.readFileSync(path.join(root, "src/pc/status_http_server.cpp"), "utf8");
const statusPageSource = fs.readFileSync(path.join(root, "src/pc/pc_status_page.cpp"), "utf8");
const adbBridgeSource = fs.readFileSync(path.join(root, "src/pc/adb_bridge.cpp"), "utf8");

assert(header.includes("class PcWebViewWindow"), "WebView shell should expose PcWebViewWindow");
assert(header.includes("show(const std::wstring& url)"), "WebView shell should show a URL");
assert(header.includes("setCloseCallback"), "WebView shell should notify the PC runtime when the window closes");
assert(source.includes("CreateCoreWebView2EnvironmentWithOptions"), "WebView shell should initialize WebView2");
assert(source.includes("Navigate(url.c_str())"), "WebView shell should navigate to the local console URL");
assert(source.includes("WM_DESTROY"), "WebView shell should handle window destruction");
assert(source.includes("WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX"), "WebView shell should use a borderless native window");
assert(source.includes("add_WebMessageReceived"), "WebView shell should handle window controls from the web UI");
assert(source.includes("runMessageLoop"), "WebView shell should own a UI message loop");
assert(source.includes("startDetached"), "WebView shell should support detached startup");

assert(main.includes("#include \"pc_webview_window.h\""), "PC main should include the WebView shell");
assert(main.includes("#include \"adb_bridge.h\""), "PC main should include the bundled ADB bridge");
assert(main.includes("--no-webview"), "PC main should support disabling the WebView window");
assert(main.includes("startPcWebView"), "PC main should start the WebView shell after status server startup");
assert(main.includes("stopPcWebView"), "PC main should stop the WebView shell during shutdown");
assert(main.includes("prepareAdbForward()"), "PC main should prepare ADB forwarding during startup and reconnect");
assert(main.includes("enableDpiAwareness()"), "PC main should enable DPI awareness before creating the WebView window");
assert(main.includes("DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2"), "PC main should prefer per-monitor DPI awareness for crisp WebView rendering");
assert(main.includes("g_running = false") && main.includes("g_wasapiCapture.stop()"), "PC main should exit the backend when the WebView window is closed");
assert(main.includes("http://127.0.0.1:8766/"), "PC main should load the existing local control page");
assert(!main.includes("Press Enter to retry"), "PC GUI startup should not wait on stdin when the phone is unavailable");
assert(main.includes("Keep this window open and use Reconnect Phone"), "PC GUI startup should keep the window open when the phone is unavailable");
assert(!statusPageSource.includes("drag-hint"), "PC web UI should not show explicit drag instructions");
assert(statusPageSource.includes("document.querySelector(\".topbar\").addEventListener(\"mousedown\""), "PC web UI should only start window drag from the top bar");
assert(statusPageSource.includes("!event.target.closest(\"button, .poll\")"), "PC web UI should not drag from top bar controls");
assert(statusPageSource.includes("id=\"runtime-panel\""), "PC web UI should identify the runtime panel for device menu layering");
assert(statusPageSource.includes("id=\"side-panel\""), "PC web UI should identify the side panel for device menu layering");
assert(statusPageSource.includes("position: fixed"), "PC web UI device menu should render as a fixed floating layer");
assert(statusPageSource.includes("z-index: 10000"), "PC web UI device menu should sit above later panels");
assert(statusPageSource.includes("document.body.appendChild($(\"audio-device-menu\"))"), "PC web UI should move the device menu to the body");
assert(statusPageSource.includes("function positionDeviceMenu()"), "PC web UI should position the floating device menu from the button rect");
assert(statusPageSource.includes(".panel.device-open"), "PC web UI should lift the runtime panel while the device menu is open");
assert(statusPageSource.includes(".side.device-open"), "PC web UI should lift the side panel while the device menu is open");
assert(statusPageSource.includes("$(\"side-panel\").classList.toggle(\"device-open\", open)"), "PC web UI should toggle side panel layering with menu state");
assert(statusPageSource.includes("$(\"runtime-panel\").classList.toggle(\"device-open\", open)"), "PC web UI should toggle device menu layering with menu state");

assert(buildPc.includes("pc_webview_window.cpp"), "Direct MSVC build should compile the WebView shell");
assert(buildPc.includes("adb_bridge.cpp"), "Direct MSVC build should compile the bundled ADB bridge");
assert(buildPc.includes("WEBVIEW2_DIR"), "Direct MSVC build should locate the WebView2 NuGet SDK");
assert(buildPc.includes("WebView2LoaderStatic.lib"), "Direct MSVC build should link the static WebView2 loader");
assert(buildPc.includes("/SUBSYSTEM:WINDOWS") && buildPc.includes("/ENTRY:mainCRTStartup"), "Direct MSVC build should produce a GUI executable without a console window");
assert(buildPc.includes("advapi32.lib"), "Direct MSVC build should link WebView2 static loader dependencies");
assert(cmake.includes("pc_webview_window.cpp"), "CMake build should include the WebView shell source");
assert(cmake.includes("adb_bridge.cpp"), "CMake build should include the bundled ADB bridge source");
assert(cmake.includes("WebView2LoaderStatic"), "CMake build should link the static WebView2 loader");
assert(cmake.includes("/SUBSYSTEM:WINDOWS") && cmake.includes("/ENTRY:mainCRTStartup"), "CMake build should produce a GUI executable without a console window");
assert(cmake.includes("advapi32"), "CMake build should link WebView2 static loader dependencies");
assert(verifyRelease.includes("pc_webview_shell.test.js"), "release static tests should include WebView shell coverage");

assert(
  statusServerSource.includes("std::async(std::launch::async") &&
    statusServerSource.includes("pruneCompletedClientTasks") &&
    statusServerSource.includes("m_clientTasks") &&
    statusServerSource.includes("SO_RCVTIMEO"),
  "status HTTP server should handle WebView2 and status polling connections concurrently without accumulating completed client workers",
);
assert(
  adbBridgeSource.includes("CREATE_NO_WINDOW") &&
    adbBridgeSource.includes("forward tcp:") &&
    adbBridgeSource.includes("devices") &&
    adbBridgeSource.includes("start-server"),
  "ADB bridge should find bundled adb and establish USB port forwarding without opening a console window",
);

console.log("pc_webview_shell tests passed");
