const assert = require("assert");
const {
  characterErrorRate,
  formatAccuracyMarkdown,
  formatAccuracyTsv,
  parseTimingMs,
} = require("./pc_wav_suite");

assert.strictEqual(parseTimingMs("[Timing] PC send-to-text: 134 ms\n[Text] 开饭"), "134");
assert.strictEqual(characterErrorRate("开饭时间", "开饭时间"), 0);
assert.strictEqual(characterErrorRate("开饭时间", "开饭"), 0.5);
assert.strictEqual(characterErrorRate("", "开饭"), 1);

const rows = [
  {
    file: "test_audio/zh.wav",
    mode: "wav",
    expected: "开饭时间早上9点至下午5点。",
    actual: "开饭时间早上9点至下午5点。",
    dryRunText: "开饭时间早上9点至下午5点。",
    elapsedMs: "134",
    cer: 0,
    ok: true,
    reason: "pass",
  },
  {
    file: "test_audio/zh.wav",
    mode: "wav-vad",
    expected: "开饭时间早上9点至下午5点。",
    actual: "开饭时间早上9点。",
    dryRunText: "开饭时间早上9点。",
    elapsedMs: "220",
    cer: 0.5,
    ok: false,
    reason: "expected substring missing",
  },
];

const tsv = formatAccuracyTsv(rows);
assert(tsv.includes("file\tmode\tok\tcer\telapsed_ms\texpected\tactual"));
assert(tsv.includes("0.500"));

const md = formatAccuracyMarkdown(rows);
assert(md.includes("| file | mode | ok | CER | elapsed_ms | expected | actual |"));
assert(md.includes("开饭时间早上9点至下午5点。"));

console.log("pc_accuracy_report tests passed");
