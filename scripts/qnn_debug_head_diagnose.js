#!/usr/bin/env node

const fs = require("fs");
const path = require("path");
const crypto = require("crypto");

const root = process.env.STT_ROOT || path.resolve(__dirname, "..");
const defaultDebugDir = path.join(
  root,
  "build",
  "test-results",
  "qnn-net-run",
  "debug-head",
);

const args = process.argv.slice(2);
const strict = args.includes("--strict");
const debugDirArg = args.find((arg) => !arg.startsWith("--"));
const debugDir = debugDirArg ? path.resolve(debugDirArg) : defaultDebugDir;

function sha256(buffer) {
  return crypto.createHash("sha256").update(buffer).digest("hex").toUpperCase();
}

function readRequired(filePath) {
  if (!fs.existsSync(filePath)) {
    throw new Error(`Missing required file: ${filePath}`);
  }
  return fs.readFileSync(filePath);
}

function stats(buffer) {
  let nonZero8 = 0;
  let min8 = 255;
  let max8 = 0;
  for (const value of buffer) {
    if (value !== 0) nonZero8 += 1;
    if (value < min8) min8 = value;
    if (value > max8) max8 = value;
  }

  let nonZero16 = 0;
  let min16 = 65535;
  let max16 = 0;
  const total16 = Math.floor(buffer.length / 2);
  for (let i = 0; i < total16; i += 1) {
    const value = buffer.readUInt16LE(i * 2);
    if (value !== 0) nonZero16 += 1;
    if (value < min16) min16 = value;
    if (value > max16) max16 = value;
  }

  return {
    bytes: buffer.length,
    u8: { nonZero: nonZero8, total: buffer.length, min: min8, max: max8 },
    u16le: { nonZero: nonZero16, total: total16, min: min16, max: max16 },
  };
}

function readTensorStats(name) {
  const filePath = path.join(debugDir, name);
  return { file: name, ...stats(readRequired(filePath)) };
}

function readProgramHeaders(buffer) {
  const phoff = Number(buffer.readBigUInt64LE(0x20));
  const phentsize = buffer.readUInt16LE(0x36);
  const phnum = buffer.readUInt16LE(0x38);
  const headers = [];

  for (let i = 0; i < phnum; i += 1) {
    const offset = phoff + i * phentsize;
    headers.push({
      type: buffer.readUInt32LE(offset),
      offset: Number(buffer.readBigUInt64LE(offset + 8)),
      filesz: Number(buffer.readBigUInt64LE(offset + 32)),
    });
  }

  return headers;
}

function auditElfRepair() {
  const originalPath = path.join(root, "models", "sensevoice", "libmodel.so");
  const fixedPath = path.join(root, "build", "qnn-model-fixed", "libmodel.so");
  const original = readRequired(originalPath);
  const fixed = readRequired(fixedPath);

  const differingOffsets = [];
  for (let i = 0; i < original.length; i += 1) {
    if (original[i] !== fixed[i]) differingOffsets.push(i);
  }

  const loadEnds = readProgramHeaders(original)
    .filter((header) => header.type === 1)
    .map((header) => header.offset + header.filesz);
  const maxLoadEnd = Math.max(...loadEnds);
  const changedInsideLoad = differingOffsets.filter((offset) => offset < maxLoadEnd);

  return {
    originalBytes: original.length,
    fixedBytes: fixed.length,
    extraBytes: fixed.length - original.length,
    originalSha256: sha256(original),
    fixedSha256: sha256(fixed),
    maxLoadEnd,
    differingOffsets: differingOffsets.map((offset) => `0x${offset.toString(16)}`),
    changedInsideLoad: changedInsideLoad.map((offset) => `0x${offset.toString(16)}`),
  };
}

function main() {
  const tensors = [
    readTensorStats("_encoder_tp_norm_LayerNormalization_output_0_native.raw"),
    readTensorStats("_ctc_lo_MatMul_pre_reshape_native.raw"),
    readTensorStats("logits_fc_native.raw"),
    readTensorStats("logits_native.raw"),
  ];
  const elf = auditElfRepair();

  const ctcInput = tensors.find((tensor) =>
    tensor.file.includes("_ctc_lo_MatMul_pre_reshape"),
  );
  const logitsFc = tensors.find((tensor) => tensor.file === "logits_fc_native.raw");
  const logits = tensors.find((tensor) => tensor.file === "logits_native.raw");

  const headInputNonZero = ctcInput.u16le.nonZero > 0;
  const logitsFcZero = logitsFc.u16le.nonZero === 0;
  const logitsZero = logits.u16le.nonZero === 0;
  const elfRuntimeOnlyHeaderChanged =
    elf.extraBytes > 0 &&
    elf.changedInsideLoad.every((offset) =>
      ["0x28", "0x29", "0x3c", "0x3e"].includes(offset),
    );

  const result = {
    debugDir,
    tensors,
    elf,
    diagnosis: {
      headInputNonZero,
      logitsFcZero,
      logitsZero,
      elfRuntimeOnlyHeaderChanged,
      blocker:
        headInputNonZero && logitsFcZero && logitsZero
          ? "final ctc_lo output is all zero while its input is non-zero"
          : "",
    },
  };

  console.log(JSON.stringify(result, null, 2));

  if (strict && result.diagnosis.blocker) {
    process.exit(1);
  }
}

try {
  main();
} catch (error) {
  console.error(`[Error] ${error.message}`);
  process.exit(1);
}

