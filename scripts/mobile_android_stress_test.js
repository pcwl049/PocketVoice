#!/usr/bin/env node

const fs = require("fs");
const net = require("net");
const path = require("path");
const { execFileSync } = require("child_process");

const MAGIC = 0x5354;
const MESSAGE_TYPE_AUDIO = 0x01;
const MESSAGE_TYPE_TEXT = 0x02;
const FLAG_HAS_SEGMENT_ID = 0x04;

function usage(exitCode) {
  const script = path.basename(process.argv[1]);
  console.log(`Usage: node scripts/${script} [options]`);
  console.log("");
  console.log("Options:");
  console.log("  --dir <path>          Directory containing wav files");
  console.log("  --wav <path>          Single wav file to repeat");
  console.log("  --iterations <n>      Number of requests to send, default 30");
  console.log("  --host <host>         TCP host, default 127.0.0.1");
  console.log("  --port <port>         TCP port, default 27000");
  console.log("  --timeout-ms <n>      Per-request timeout, default 120000");
  console.log("  --pause-ms <n>        Pause between requests, default 500");
  console.log("  --mem-every <n>       Capture adb meminfo every n requests, default 5");
  console.log("  --adb <path>          adb path, default ANDROID_HOME/platform-tools/adb.exe");
  console.log("  --output <path>       JSON result path under build/test-results");
  console.log("  --help                Show this help");
  process.exit(exitCode);
}

function parseArgs(argv) {
  const args = {
    dir: "test_audio/generated/hotwords",
    wav: "",
    iterations: 30,
    host: "127.0.0.1",
    port: 27000,
    timeoutMs: 120000,
    pauseMs: 500,
    memEvery: 5,
    adb: "",
    output: "build/test-results/mobile-android-stress-results.json",
  };

  for (let i = 2; i < argv.length; i += 1) {
    const key = argv[i];
    if (key === "--help" || key === "-h") usage(0);
    const value = argv[i + 1];
    if (!value) throw new Error(`Missing value for ${key}`);
    i += 1;
    if (key === "--dir") args.dir = value;
    else if (key === "--wav") args.wav = value;
    else if (key === "--iterations") args.iterations = Number(value);
    else if (key === "--host") args.host = value;
    else if (key === "--port") args.port = Number(value);
    else if (key === "--timeout-ms") args.timeoutMs = Number(value);
    else if (key === "--pause-ms") args.pauseMs = Number(value);
    else if (key === "--mem-every") args.memEvery = Number(value);
    else if (key === "--adb") args.adb = value;
    else if (key === "--output") args.output = value;
    else throw new Error(`Unknown option: ${key}`);
  }

  for (const [name, value] of [
    ["iterations", args.iterations],
    ["port", args.port],
    ["timeoutMs", args.timeoutMs],
    ["pauseMs", args.pauseMs],
    ["memEvery", args.memEvery],
  ]) {
    if (!Number.isInteger(value) || value <= 0) {
      throw new Error(`Invalid ${name}: ${value}`);
    }
  }

  if (!args.adb) {
    const androidHome = process.env.ANDROID_HOME || process.env.ANDROID_SDK_ROOT || "D:/Android/Sdk";
    args.adb = path.join(androidHome, "platform-tools", process.platform === "win32" ? "adb.exe" : "adb");
  }

  return args;
}

function readWav16Mono(wavPath) {
  const buffer = fs.readFileSync(wavPath);
  if (buffer.toString("ascii", 0, 4) !== "RIFF" || buffer.toString("ascii", 8, 12) !== "WAVE") {
    throw new Error(`Not a WAV file: ${wavPath}`);
  }

  const format = buffer.readUInt16LE(20);
  const channels = buffer.readUInt16LE(22);
  const sampleRate = buffer.readUInt32LE(24);
  const bitsPerSample = buffer.readUInt16LE(34);
  if (format !== 1 || channels !== 1 || bitsPerSample !== 16) {
    throw new Error(`Unsupported wav format in ${wavPath}; expected 16-bit mono PCM`);
  }

  let offset = 12;
  let dataOffset = -1;
  let dataLength = 0;
  while (offset + 8 <= buffer.length) {
    const id = buffer.toString("ascii", offset, offset + 4);
    const length = buffer.readUInt32LE(offset + 4);
    if (id === "data") {
      dataOffset = offset + 8;
      dataLength = length;
      break;
    }
    offset += 8 + length + (length % 2);
  }
  if (dataOffset < 0) throw new Error(`WAV data chunk not found: ${wavPath}`);

  const samples = new Float32Array(Math.floor(dataLength / 2));
  for (let i = 0; i < samples.length; i += 1) {
    samples[i] = buffer.readInt16LE(dataOffset + i * 2) / 32768;
  }
  return { sampleRate, samples };
}

function makeAudioPacket(samples, sampleRate, segmentId) {
  const payloadLength = 4 + 2 + samples.length * 4 + 4;
  const buffer = Buffer.alloc(8 + payloadLength);
  let offset = 0;
  buffer.writeUInt16BE(MAGIC, offset);
  offset += 2;
  buffer.writeUInt8(MESSAGE_TYPE_AUDIO, offset);
  offset += 1;
  buffer.writeUInt8(0x01 | FLAG_HAS_SEGMENT_ID, offset);
  offset += 1;
  buffer.writeUInt32BE(payloadLength, offset);
  offset += 4;
  buffer.writeUInt32BE(sampleRate, offset);
  offset += 4;
  buffer.writeUInt16BE(1, offset);
  offset += 2;
  for (let i = 0; i < samples.length; i += 1, offset += 4) {
    buffer.writeFloatLE(samples[i], offset);
  }
  buffer.writeUInt32BE(segmentId, offset);
  return buffer;
}

function parseTextPayload(payload, flags) {
  let offset = 0;
  const textLength = payload.readUInt32BE(offset);
  offset += 4;
  const text = payload.subarray(offset, offset + textLength).toString("utf8");
  offset += textLength;
  const emotion = offset < payload.length ? payload.readUInt8(offset) : null;
  if (offset < payload.length) offset += 1;
  const event = offset < payload.length ? payload.readUInt8(offset) : null;
  if (offset < payload.length) offset += 1;
  let segmentId = null;
  if ((flags & FLAG_HAS_SEGMENT_ID) && offset + 4 <= payload.length) {
    segmentId = payload.readUInt32BE(offset);
  }
  return { text, emotion, event, segmentId };
}

function sendAudio({ host, port, timeoutMs, wav, segmentId }) {
  const packet = makeAudioPacket(wav.samples, wav.sampleRate, segmentId);
  return new Promise((resolve, reject) => {
    const startedAt = Date.now();
    const socket = net.createConnection({ host, port });
    let received = Buffer.alloc(0);
    let finished = false;
    const timeout = setTimeout(() => {
      socket.destroy();
      reject(new Error(`Timed out after ${timeoutMs}ms`));
    }, timeoutMs);

    function finish(value) {
      if (finished) return;
      finished = true;
      clearTimeout(timeout);
      socket.end();
      resolve({ ...value, ms: Date.now() - startedAt });
    }

    socket.on("connect", () => socket.write(packet));
    socket.on("data", (chunk) => {
      received = Buffer.concat([received, chunk]);
      while (received.length >= 8) {
        const magic = received.readUInt16BE(0);
        const type = received.readUInt8(2);
        const flags = received.readUInt8(3);
        const length = received.readUInt32BE(4);
        if (received.length < 8 + length) return;
        const payload = received.subarray(8, 8 + length);
        received = received.subarray(8 + length);
        if (magic !== MAGIC) {
          socket.destroy();
          reject(new Error(`Invalid magic: 0x${magic.toString(16)}`));
          return;
        }
        if (type === MESSAGE_TYPE_TEXT) {
          finish(parseTextPayload(payload, flags));
        }
      }
    });
    socket.on("error", reject);
    socket.on("close", () => {
      if (!finished) {
        clearTimeout(timeout);
        reject(new Error("Socket closed before text response"));
      }
    });
  });
}

function captureMeminfo(adb) {
  try {
    const output = execFileSync(adb, ["shell", "dumpsys", "meminfo", "com.stt.mobile"], {
      encoding: "utf8",
      timeout: 30000,
    });
    const total = output.match(/TOTAL PSS:\s+(\d+)/);
    const nativeHeap = output.match(/Native Heap\s+(\d+)/);
    const rss = output.match(/TOTAL RSS:\s+(\d+)/);
    const swap = output.match(/TOTAL SWAP PSS:\s+(\d+)/);
    return {
      ok: true,
      totalPssKb: total ? Number(total[1]) : null,
      nativeHeapPssKb: nativeHeap ? Number(nativeHeap[1]) : null,
      totalRssKb: rss ? Number(rss[1]) : null,
      totalSwapPssKb: swap ? Number(swap[1]) : null,
    };
  } catch (error) {
    return { ok: false, error: error.message };
  }
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function getWavPaths(args) {
  if (args.wav) return [args.wav];
  return fs.readdirSync(args.dir)
    .filter((name) => name.toLowerCase().endsWith(".wav"))
    .sort()
    .map((name) => path.join(args.dir, name));
}

async function main() {
  const args = parseArgs(process.argv);
  const wavPaths = getWavPaths(args);
  if (wavPaths.length === 0) throw new Error("No wav files found");

  const wavs = wavPaths.map((wavPath) => {
    const wav = readWav16Mono(wavPath);
    return {
      path: wavPath,
      name: path.basename(wavPath),
      durationSeconds: wav.samples.length / wav.sampleRate,
      wav,
    };
  });

  fs.mkdirSync(path.dirname(args.output), { recursive: true });
  const summary = {
    startedAt: new Date().toISOString(),
    args: { ...args, adb: args.adb },
    results: [],
    memory: [{ iteration: 0, ...captureMeminfo(args.adb) }],
  };

  for (let i = 1; i <= args.iterations; i += 1) {
    const item = wavs[(i - 1) % wavs.length];
    const row = {
      iteration: i,
      file: item.name,
      durationSeconds: item.durationSeconds,
      ok: false,
      ms: null,
      text: "",
      error: "",
    };
    try {
      const result = await sendAudio({
        host: args.host,
        port: args.port,
        timeoutMs: args.timeoutMs,
        wav: item.wav,
        segmentId: i,
      });
      row.ok = true;
      row.ms = result.ms;
      row.text = result.text;
    } catch (error) {
      row.error = error.message;
    }
    summary.results.push(row);
    console.log(`[${row.ok ? "ok" : "fail"}] #${i} ${row.file} ${row.ms ?? ""}ms ${row.text || row.error}`);

    if (i % args.memEvery === 0 || i === args.iterations) {
      summary.memory.push({ iteration: i, ...captureMeminfo(args.adb) });
    }
    if (i < args.iterations) await sleep(args.pauseMs);
  }

  summary.finishedAt = new Date().toISOString();
  fs.writeFileSync(args.output, JSON.stringify(summary, null, 2), "utf8");
  const failures = summary.results.filter((row) => !row.ok).length;
  if (failures > 0) process.exitCode = 1;
}

main().catch((error) => {
  console.error(`[Error] ${error.message}`);
  process.exit(1);
});
