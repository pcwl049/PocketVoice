#!/usr/bin/env node

const fs = require("fs");
const net = require("net");
const path = require("path");

const MAGIC = 0x5354;
const MESSAGE_TYPE_AUDIO = 0x01;
const MESSAGE_TYPE_TEXT = 0x02;
const FLAG_HAS_SEGMENT_ID = 0x04;

function usage(exitCode) {
  const script = path.basename(process.argv[1]);
  console.log(`Usage: node scripts/${script} <wav-path> [host] [port]`);
  console.log("");
  console.log("Sends a 16-bit mono WAV file to the PocketVoice Android service and prints the returned text.");
  console.log("Defaults: host=127.0.0.1 port=27000");
  console.log("Set STT_SEND_WAV_TIMEOUT_MS to override the default 30000ms response timeout.");
  process.exit(exitCode);
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

  if (format !== 1) {
    throw new Error(`Unsupported WAV format ${format}; expected PCM format 1`);
  }
  if (channels !== 1) {
    throw new Error(`Unsupported channel count ${channels}; expected mono`);
  }
  if (bitsPerSample !== 16) {
    throw new Error(`Unsupported bit depth ${bitsPerSample}; expected 16-bit PCM`);
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

  if (dataOffset < 0) {
    throw new Error(`WAV data chunk not found: ${wavPath}`);
  }

  const sampleCount = Math.floor(dataLength / 2);
  const samples = new Float32Array(sampleCount);
  for (let i = 0; i < sampleCount; i += 1) {
    samples[i] = buffer.readInt16LE(dataOffset + i * 2) / 32768;
  }

  return { sampleRate, samples };
}

function makeAudioPacket(samples, sampleRate, segmentId = 1) {
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

function parseTextPayload(payload, flags = 0) {
  if (payload.length < 4) {
    throw new Error(`Text payload too short: ${payload.length}`);
  }

  let offset = 0;
  const textLength = payload.readUInt32BE(offset);
  offset += 4;

  if (offset + textLength > payload.length) {
    throw new Error(`Invalid text payload length: ${textLength}`);
  }

  const text = payload.subarray(offset, offset + textLength).toString("utf8");
  offset += textLength;

  let segmentId = null;
  const emotion = offset < payload.length ? payload.readUInt8(offset) : null;
  if (offset < payload.length) offset += 1;
  const event = offset < payload.length ? payload.readUInt8(offset) : null;
  if (offset < payload.length) offset += 1;

  if ((flags & FLAG_HAS_SEGMENT_ID) && offset + 4 <= payload.length) {
    segmentId = payload.readUInt32BE(offset);
  }

  return {
    text,
    emotion,
    event,
    segmentId,
  };
}

async function main() {
  if (process.argv.includes("--help") || process.argv.includes("-h")) {
    usage(0);
  }

  const wavPath = process.argv[2];
  if (!wavPath) {
    usage(1);
  }

  const host = process.argv[3] || "127.0.0.1";
  const port = Number(process.argv[4] || "27000");
  if (!Number.isInteger(port) || port <= 0 || port > 65535) {
    throw new Error(`Invalid port: ${process.argv[4]}`);
  }
  const timeoutMs = Number(process.env.STT_SEND_WAV_TIMEOUT_MS || "30000");
  if (!Number.isInteger(timeoutMs) || timeoutMs <= 0) {
    throw new Error(`Invalid STT_SEND_WAV_TIMEOUT_MS: ${process.env.STT_SEND_WAV_TIMEOUT_MS}`);
  }

  const wav = readWav16Mono(wavPath);
  const durationSeconds = wav.samples.length / wav.sampleRate;
  const packet = makeAudioPacket(wav.samples, wav.sampleRate);

  console.log(`Connecting to ${host}:${port}`);
  console.log(`Sending ${wav.samples.length} samples at ${wav.sampleRate}Hz (${durationSeconds.toFixed(2)}s)`);

  await new Promise((resolve, reject) => {
    const socket = net.createConnection({ host, port });
    let received = Buffer.alloc(0);
    let finished = false;
    const timeout = setTimeout(() => {
      socket.destroy();
      reject(new Error(`Timed out waiting for text response after ${timeoutMs}ms`));
    }, timeoutMs);

    function finish(result) {
      if (finished) return;
      finished = true;
      clearTimeout(timeout);
      socket.end();
      resolve(result);
    }

    socket.on("connect", () => {
      socket.write(packet);
    });

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
          reject(new Error(`Invalid magic: 0x${magic.toString(16)}`));
          socket.destroy();
          return;
        }

        console.log(`Received message type=${type} flags=${flags} payload=${length} bytes`);
        if (type === MESSAGE_TYPE_TEXT) {
          const result = parseTextPayload(payload, flags);
          console.log(`Text: ${result.text}`);
          console.log(`Emotion: ${result.emotion ?? ""}`);
          console.log(`Event: ${result.event ?? ""}`);
          console.log(`Segment: ${result.segmentId ?? ""}`);
          finish(result);
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

main().catch((error) => {
  console.error(`[Error] ${error.message}`);
  process.exit(1);
});
