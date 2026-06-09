const assert = require("assert");
const fs = require("fs");
const path = require("path");

const root = path.resolve(__dirname, "..");

const brandingSvgPath = path.join(root, "src/branding/pocketvoice-icon.svg");
const manifestPath = path.join(root, "src/mobile/app/src/main/AndroidManifest.xml");
const stringsPath = path.join(root, "src/mobile/app/src/main/res/values/strings.xml");
const buildMobilePath = path.join(root, "scripts/build_mobile_apk.bat");
const resourceHeaderPath = path.join(root, "src/pc/resource.h");
const resourceScriptPath = path.join(root, "src/pc/pc_resources.rc");
const webviewWindowPath = path.join(root, "src/pc/pc_webview_window.cpp");
const verifyReleasePath = path.join(root, "scripts/verify_release.bat");

assert(fs.existsSync(brandingSvgPath), "PocketVoice should keep the approved icon SVG source in src/branding");
const brandingSvg = fs.readFileSync(brandingSvgPath, "utf8");
assert(brandingSvg.includes("Lucide audio-lines"), "icon source should record the Lucide audio-lines origin");
assert(brandingSvg.includes("#8fbd78") && brandingSvg.includes("#22251f"), "icon source should keep the approved green and dark chat bubble palette");

const manifest = fs.readFileSync(manifestPath, "utf8");
assert(manifest.includes('android:label="@string/app_name"'), "Android manifest should use the app name resource");
assert(manifest.includes('android:icon="@mipmap/ic_launcher"'), "Android manifest should point to the PocketVoice launcher icon");
assert(manifest.includes('android:roundIcon="@mipmap/ic_launcher_round"'), "Android manifest should point to the round launcher icon");

const strings = fs.readFileSync(stringsPath, "utf8");
assert(strings.includes("<string name=\"app_name\">PocketVoice</string>"), "Android app label should be PocketVoice");

const androidIconFiles = [
  "src/mobile/app/src/main/res/drawable/ic_launcher_background.xml",
  "src/mobile/app/src/main/res/drawable/ic_launcher_foreground.xml",
  "src/mobile/app/src/main/res/mipmap-anydpi/ic_launcher.xml",
  "src/mobile/app/src/main/res/mipmap-anydpi/ic_launcher_round.xml",
  "src/mobile/app/src/main/res/mipmap-anydpi-v26/ic_launcher.xml",
  "src/mobile/app/src/main/res/mipmap-anydpi-v26/ic_launcher_round.xml",
];
for (const relativePath of androidIconFiles) {
  assert(fs.existsSync(path.join(root, relativePath)), `Android icon resource missing: ${relativePath}`);
}
const foreground = fs.readFileSync(path.join(root, "src/mobile/app/src/main/res/drawable/ic_launcher_foreground.xml"), "utf8");
assert(foreground.includes("#8FBD78") && foreground.includes("#EEE8DA"), "Android foreground icon should include the approved accent and warm line colors");
const adaptiveIcon = fs.readFileSync(path.join(root, "src/mobile/app/src/main/res/mipmap-anydpi-v26/ic_launcher.xml"), "utf8");
assert(adaptiveIcon.includes("<adaptive-icon") && adaptiveIcon.includes("@drawable/ic_launcher_foreground"), "Android v26 icon should use adaptive icon resources");

const buildMobile = fs.readFileSync(buildMobilePath, "utf8");
assert(buildMobile.includes("*.flat") && !buildMobile.includes("values_*.arsc.flat"), "APK build should link all compiled resources, including launcher icons");

const resourceHeader = fs.readFileSync(resourceHeaderPath, "utf8");
const resourceScript = fs.readFileSync(resourceScriptPath, "utf8");
const webviewWindow = fs.readFileSync(webviewWindowPath, "utf8");
assert(resourceHeader.includes("#define IDI_APP_ICON 1"), "PC resources should define the application icon id");
assert(resourceScript.includes('IDI_APP_ICON ICON "assets\\\\pocketvoice.ico"'), "PC resource script should embed the PocketVoice icon");
assert(fs.existsSync(path.join(root, "src/pc/assets/pocketvoice.ico")), "PC icon file should exist");
assert(webviewWindow.includes("MAKEINTRESOURCEW(IDI_APP_ICON)"), "PC window class should load the PocketVoice icon");

const verifyRelease = fs.readFileSync(verifyReleasePath, "utf8");
assert(verifyRelease.includes("app_icon.test.js"), "release static verification should include app icon coverage");

console.log("app_icon tests passed");
