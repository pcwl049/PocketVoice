const assert = require("assert");
const fs = require("fs");
const path = require("path");

const root = path.resolve(__dirname, "..");
const pressureScriptPath = path.join(root, "scripts", "android_pressure_suite.js");
const pressureBatPath = path.join(root, "scripts", "test_android_pressure_suite.bat");

assert(fs.existsSync(pressureScriptPath), "Android pressure suite script should exist");
assert(fs.existsSync(pressureBatPath), "Android pressure suite batch wrapper should exist");

const {
  applyAudioVariant,
  buildPressureCases,
  evaluateAnyResponse,
  evaluateResponse,
  formatPressureTsv,
  makeAudioPacket,
  parseTextPayload,
  pressureExitCode,
  parsePositiveInt,
  summarizeRows,
} = require("./android_pressure_suite");

const baseRows = [
  {
    file: "test_audio/a.wav",
    mode: "wav",
    expected_contains: "alpha",
    expected_text: "alpha text",
  },
  {
    file: "test_audio/a.wav",
    mode: "wav-vad",
    expected_contains: "ignored",
    expected_text: "ignored",
  },
  {
    file: "test_audio/b.wav",
    mode: "wav",
    expected_contains: "beta",
    expected_text: "beta text",
  },
];

const cases = buildPressureCases(baseRows, 5);
assert.strictEqual(cases.length, 5);
assert.deepStrictEqual(
  cases.map((item) => item.file),
  ["test_audio/a.wav", "test_audio/b.wav", "test_audio/a.wav", "test_audio/b.wav", "test_audio/a.wav"],
);
assert.deepStrictEqual(
  cases.map((item) => item.sequence),
  [1, 2, 3, 4, 5],
);

assert.throws(() => buildPressureCases(baseRows, 0), /count must be positive/);
assert.throws(() => buildPressureCases([], 1), /No wav-mode rows/);

const samples = Float32Array.from([0, 0.25, -0.25, 1, -1]);
const repeated = applyAudioVariant(samples, 3, "repeat");
assert.deepStrictEqual([...repeated], [...samples]);

const unique = applyAudioVariant(samples, 3, "unique");
assert.notDeepStrictEqual([...unique], [...samples]);
assert(unique.every((value) => value >= -1 && value <= 1), "variant samples should remain clipped");

const packet = makeAudioPacket(Float32Array.from([0.5, -0.5]), 16000, 42);
assert.strictEqual(packet.readUInt8(3) & 0x04, 0x04);
assert.strictEqual(packet.readUInt32BE(4), 4 + 2 + 2 * 4 + 4);
assert.strictEqual(packet.readUInt32BE(packet.length - 4), 42);

const textPayload = Buffer.alloc(4 + Buffer.byteLength("ok") + 1 + 1 + 4);
let textOffset = 0;
textPayload.writeUInt32BE(2, textOffset);
textOffset += 4;
textPayload.write("ok", textOffset, "utf8");
textOffset += 2;
textPayload.writeUInt8(7, textOffset);
textOffset += 1;
textPayload.writeUInt8(1, textOffset);
textOffset += 1;
textPayload.writeUInt32BE(42, textOffset);
assert.deepStrictEqual(parseTextPayload(textPayload, 0x07), {
  text: "ok",
  emotion: 7,
  event: 1,
  segmentId: 42,
});
assert.strictEqual(parseTextPayload(textPayload.subarray(0, textPayload.length - 4), 0x03).segmentId, null);

assert.deepStrictEqual(
  evaluateResponse({
    caseItem: cases[0],
    text: "xx alpha yy",
    responseMs: 123,
    sincePreviousResponseMs: 50,
  }),
  {
    sequence: 1,
    file: "test_audio/a.wav",
    ok: true,
    expected: "alpha text",
    expectedContains: "alpha",
    actual: "xx alpha yy",
    responseMs: 123,
    sincePreviousResponseMs: 50,
    segmentId: null,
    cer: 0.7,
    reason: "pass",
    emotion: "",
    event: "",
  },
);

assert.strictEqual(
  evaluateResponse({
    caseItem: cases[1],
    text: "wrong",
    responseMs: 1,
    sincePreviousResponseMs: 1,
  }).reason,
  "expected substring missing",
);

assert.deepStrictEqual(
  evaluateAnyResponse({
    cases,
    responseIndex: 3,
    text: "beta and more",
    responseMs: 42,
    sincePreviousResponseMs: 11,
  }),
  {
    sequence: 3,
    file: "test_audio/b.wav",
    ok: true,
    expected: "beta text",
    expectedContains: "beta",
    actual: "beta and more",
    responseMs: 42,
    sincePreviousResponseMs: 11,
    segmentId: null,
    cer: 0.8888888888888888,
    reason: "pass",
    emotion: "",
    event: "",
  },
);

assert.strictEqual(
  evaluateAnyResponse({
    cases,
    responseIndex: 4,
    text: "unknown",
    responseMs: 1,
    sincePreviousResponseMs: 1,
  }).reason,
  "no expected substring matched",
);

const tsv = formatPressureTsv([
  {
    sequence: 1,
    file: "test_audio/a.wav",
    ok: true,
    responseMs: 100,
    sincePreviousResponseMs: 0,
    segmentId: 1,
    expectedContains: "alpha",
    actual: "alpha",
    cer: 0,
    reason: "pass",
    emotion: 7,
    event: 1,
  },
]);
assert(tsv.startsWith("sequence\tsegment_id\tfile\tok\tresponse_ms"));
assert(tsv.includes("test_audio/a.wav"));

assert.strictEqual(parsePositiveInt("7", "count"), 7);
assert.throws(() => parsePositiveInt("0", "count"), /count must be positive/);

assert.deepStrictEqual(
  summarizeRows(
    [
      { ok: true, responseMs: 100 },
      { ok: false, responseMs: 300 },
    ],
    500,
    4,
  ),
  {
    passed: 1,
    total: 2,
    sent: 4,
    droppedOrMissing: 2,
    totalElapsedMs: 500,
    maxResponseMs: 300,
    avgResponseMs: 200,
  },
);

assert.strictEqual(pressureExitCode({ passed: 4, total: 4 }, { count: 4, minResponses: 4 }), 0);
assert.strictEqual(pressureExitCode({ passed: 2, total: 2 }, { count: 4, minResponses: 2 }), 0);
assert.strictEqual(pressureExitCode({ passed: 1, total: 2 }, { count: 4, minResponses: 2 }), 1);
assert.strictEqual(pressureExitCode({ passed: 1, total: 1 }, { count: 4, minResponses: 2 }), 1);

const bat = fs.readFileSync(pressureBatPath, "utf8");
assert(bat.includes("android_pressure_suite.js"), "batch wrapper should invoke android_pressure_suite.js");

const verify = fs.readFileSync(path.join(root, "scripts", "verify_release.bat"), "utf8");
assert(
  verify.includes("android_pressure_suite.test.js"),
  "release static verification should include the Android pressure script test",
);

console.log("android_pressure_suite tests passed");
