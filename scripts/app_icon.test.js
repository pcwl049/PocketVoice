const assert = require("assert");
const fs = require("fs");
const path = require("path");
const zlib = require("zlib");

const root = path.resolve(__dirname, "..");

const brandingSvgPath = path.join(root, "src/branding/pocketvoice-icon.svg");
const manifestPath = path.join(root, "src/mobile/app/src/main/AndroidManifest.xml");
const stringsPath = path.join(root, "src/mobile/app/src/main/res/values/strings.xml");
const buildMobilePath = path.join(root, "scripts/build_mobile_apk.bat");
const resourceHeaderPath = path.join(root, "src/pc/resource.h");
const resourceScriptPath = path.join(root, "src/pc/pc_resources.rc");
const webviewWindowPath = path.join(root, "src/pc/pc_webview_window.cpp");
const verifyReleasePath = path.join(root, "scripts/verify_release.bat");

function paethPredictor(left, above, upperLeft) {
  const p = left + above - upperLeft;
  const pa = Math.abs(p - left);
  const pb = Math.abs(p - above);
  const pc = Math.abs(p - upperLeft);
  if (pa <= pb && pa <= pc) return left;
  if (pb <= pc) return above;
  return upperLeft;
}

function decodePng(buffer) {
  assert(buffer.subarray(0, 8).equals(Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a])), "ICO image should contain PNG entries");
  let offset = 8;
  let width = 0;
  let height = 0;
  let colorType = 0;
  let bitDepth = 0;
  let palette = null;
  let transparency = null;
  const idat = [];

  while (offset < buffer.length) {
    const length = buffer.readUInt32BE(offset);
    const type = buffer.toString("ascii", offset + 4, offset + 8);
    const data = buffer.subarray(offset + 8, offset + 8 + length);
    if (type === "IHDR") {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      bitDepth = data[8];
      colorType = data[9];
      assert.strictEqual(bitDepth, 8, "icon PNG entries should use 8-bit channels");
    } else if (type === "PLTE") {
      palette = [];
      for (let i = 0; i < data.length; i += 3) {
        palette.push([data[i], data[i + 1], data[i + 2], 255]);
      }
    } else if (type === "tRNS") {
      transparency = data;
    } else if (type === "IDAT") {
      idat.push(data);
    } else if (type === "IEND") {
      break;
    }
    offset += 12 + length;
  }

  const bytesPerPixel = colorType === 6 ? 4 : colorType === 2 ? 3 : colorType === 3 ? 1 : 0;
  assert(bytesPerPixel > 0, `unsupported icon PNG color type: ${colorType}`);
  const rowBytes = width * bytesPerPixel;
  const inflated = zlib.inflateSync(Buffer.concat(idat));
  const rows = [];
  let inputOffset = 0;
  let previous = Buffer.alloc(rowBytes);

  for (let y = 0; y < height; y += 1) {
    const filter = inflated[inputOffset];
    inputOffset += 1;
    const row = Buffer.from(inflated.subarray(inputOffset, inputOffset + rowBytes));
    inputOffset += rowBytes;
    for (let x = 0; x < rowBytes; x += 1) {
      const left = x >= bytesPerPixel ? row[x - bytesPerPixel] : 0;
      const above = previous[x] || 0;
      const upperLeft = x >= bytesPerPixel ? previous[x - bytesPerPixel] : 0;
      if (filter === 1) row[x] = (row[x] + left) & 0xff;
      else if (filter === 2) row[x] = (row[x] + above) & 0xff;
      else if (filter === 3) row[x] = (row[x] + Math.floor((left + above) / 2)) & 0xff;
      else if (filter === 4) row[x] = (row[x] + paethPredictor(left, above, upperLeft)) & 0xff;
      else assert.strictEqual(filter, 0, `unsupported PNG filter: ${filter}`);
    }
    rows.push(row);
    previous = row;
  }

  return {
    width,
    height,
    pixelAt(x, y) {
      const row = rows[y];
      const index = x * bytesPerPixel;
      if (colorType === 6) return [row[index], row[index + 1], row[index + 2], row[index + 3]];
      if (colorType === 2) return [row[index], row[index + 1], row[index + 2], 255];
      const paletteIndex = row[index];
      const rgba = palette[paletteIndex].slice();
      if (transparency && paletteIndex < transparency.length) rgba[3] = transparency[paletteIndex];
      return rgba;
    },
  };
}

function decodeDib(buffer, fallbackWidth, fallbackHeight) {
  const headerSize = buffer.readUInt32LE(0);
  assert(headerSize >= 40, "ICO DIB entries should use a BITMAPINFOHEADER-compatible header");
  const width = Math.abs(buffer.readInt32LE(4)) || fallbackWidth;
  const storedHeight = Math.abs(buffer.readInt32LE(8));
  const height = storedHeight === fallbackHeight * 2 ? fallbackHeight : storedHeight;
  const planes = buffer.readUInt16LE(12);
  const bitCount = buffer.readUInt16LE(14);
  const compression = buffer.readUInt32LE(16);
  assert.strictEqual(planes, 1, "ICO DIB entries should use one color plane");
  assert.strictEqual(bitCount, 32, "ICO DIB entries should use 32-bit pixels");
  assert.strictEqual(compression, 0, "ICO DIB entries should be uncompressed");

  const pixelOffset = headerSize;
  const rowBytes = width * 4;
  return {
    width,
    height,
    pixelAt(x, y) {
      const bottomUpY = height - 1 - y;
      const index = pixelOffset + bottomUpY * rowBytes + x * 4;
      const blue = buffer[index];
      const green = buffer[index + 1];
      const red = buffer[index + 2];
      const alpha = buffer[index + 3];
      return [red, green, blue, alpha];
    },
  };
}

function decodeIcoImage(entry) {
  if (entry.data.subarray(0, 8).equals(Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]))) {
    return decodePng(entry.data);
  }
  return decodeDib(entry.data, entry.width, entry.height);
}

function readIcoPngEntries(filePath) {
  const ico = fs.readFileSync(filePath);
  assert.strictEqual(ico.readUInt16LE(0), 0, "ICO reserved header should be zero");
  assert.strictEqual(ico.readUInt16LE(2), 1, "ICO should contain icon images");
  const count = ico.readUInt16LE(4);
  const entries = [];
  for (let i = 0; i < count; i += 1) {
    const offset = 6 + i * 16;
    const width = ico[offset] || 256;
    const height = ico[offset + 1] || 256;
    const size = ico.readUInt32LE(offset + 8);
    const imageOffset = ico.readUInt32LE(offset + 12);
    entries.push({ width, height, data: ico.subarray(imageOffset, imageOffset + size) });
  }
  return entries;
}

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
const legacyIcon = fs.readFileSync(path.join(root, "src/mobile/app/src/main/res/mipmap-anydpi/ic_launcher.xml"), "utf8");
assert(
  legacyIcon.includes("<layer-list") &&
    legacyIcon.includes("@drawable/ic_launcher_background") &&
    legacyIcon.includes("@drawable/ic_launcher_foreground"),
  "Android legacy launcher icon should layer the dark background behind the foreground",
);
const adaptiveIcon = fs.readFileSync(path.join(root, "src/mobile/app/src/main/res/mipmap-anydpi-v26/ic_launcher.xml"), "utf8");
assert(adaptiveIcon.includes("<adaptive-icon") && adaptiveIcon.includes("@drawable/ic_launcher_foreground"), "Android v26 icon should use adaptive icon resources");

const buildMobile = fs.readFileSync(buildMobilePath, "utf8");
assert(buildMobile.includes("*.flat") && !buildMobile.includes("values_*.arsc.flat"), "APK build should link all compiled resources, including launcher icons");

const resourceHeader = fs.readFileSync(resourceHeaderPath, "utf8");
const resourceScript = fs.readFileSync(resourceScriptPath, "utf8");
const webviewWindow = fs.readFileSync(webviewWindowPath, "utf8");
assert(resourceHeader.includes("#define IDI_APP_ICON 1"), "PC resources should define the application icon id");
assert(resourceScript.includes('IDI_APP_ICON ICON "assets\\\\pocketvoice.ico"'), "PC resource script should embed the PocketVoice icon");
const pcIconPath = path.join(root, "src/pc/assets/pocketvoice.ico");
assert(fs.existsSync(pcIconPath), "PC icon file should exist");
for (const entry of readIcoPngEntries(pcIconPath)) {
  const png = decodeIcoImage(entry);
  for (const [x, y] of [
    [0, 0],
    [png.width - 1, 0],
    [0, png.height - 1],
    [png.width - 1, png.height - 1],
  ]) {
    const [red, green, blue, alpha] = png.pixelAt(x, y);
    const opaqueWhite = alpha > 240 && red > 240 && green > 240 && blue > 240;
    assert(!opaqueWhite, `PC icon ${entry.width}x${entry.height} should not have opaque white corner pixels`);
  }
}
assert(webviewWindow.includes("MAKEINTRESOURCEW(IDI_APP_ICON)"), "PC window class should load the PocketVoice icon");

const verifyRelease = fs.readFileSync(verifyReleasePath, "utf8");
assert(verifyRelease.includes("app_icon.test.js"), "release static verification should include app icon coverage");

console.log("app_icon tests passed");
