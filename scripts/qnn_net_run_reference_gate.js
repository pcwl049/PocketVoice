#!/usr/bin/env node

const fs = require("fs");
const path = require("path");
const { spawnSync } = require("child_process");

const { inferHtpArch } = require("./check_qnn_prereqs");

const root = process.env.STT_ROOT || path.resolve(__dirname, "..");
const adb = process.env.ADB || "D:\\Android\\Sdk\\platform-tools\\adb.exe";
const qnnSdk = process.env.QNN_SDK_ROOT || "G:\\Program Files\\qairt\\2.45.0.260326";
const sandbox = process.env.STT_QNN_NET_RUN_DIR || "/data/local/tmp/stt-qnn-run-gate";
const args = process.argv.slice(2);
const reportOnly = args.includes("--report-only");

function fail(message) {
  console.error(`[Error] ${message}`);
  process.exit(1);
}

function run(command, commandArgs, options = {}) {
  const result = spawnSync(command, commandArgs, {
    cwd: root,
    encoding: "utf8",
    windowsHide: true,
    ...options,
  });
  if (result.status !== 0) {
    fail(
      `${command} ${commandArgs.join(" ")} failed\n${result.stdout}${result.stderr}`.trim(),
    );
  }
  return result.stdout.trim();
}

function requireFile(filePath, label = filePath) {
  if (!fs.existsSync(filePath) || !fs.statSync(filePath).isFile()) {
    fail(`Missing ${label}: ${filePath}`);
  }
  return filePath;
}

function readDeviceSoc() {
  requireFile(adb, "adb");
  const devices = run(adb, ["devices"]);
  const device = devices
    .split(/\r?\n/)
    .map((line) => line.trim())
    .find((line) => line.endsWith("\tdevice"));
  if (!device) fail("No connected adb device");
  return run(adb, ["shell", "getprop", "ro.soc.model"]).trim().toUpperCase();
}

function readSocId(soc) {
  const header = requireFile(path.join(qnnSdk, "include", "QNN", "QnnTypes.h"));
  const text = fs.readFileSync(header, "utf8");
  const match = text.match(new RegExp(`QNN_SOC_MODEL_${soc}\\s*=\\s*(\\d+)`));
  if (!match) fail(`QNN SoC enum missing for ${soc}`);
  return Number(match[1]);
}

function push(local, remoteName) {
  run(adb, ["push", local, `${sandbox}/${remoteName}`]);
}

function countTokens() {
  const tokens = requireFile(path.join(root, "models", "sensevoice", "tokens.txt"));
  return fs
    .readFileSync(tokens, "utf8")
    .split(/\r?\n/)
    .filter((line) => line.trim()).length;
}

function loadTokens() {
  const tokens = requireFile(path.join(root, "models", "sensevoice", "tokens.txt"));
  const ans = new Map();
  fs.readFileSync(tokens, "utf8")
    .split(/\r?\n/)
    .forEach((line, fallbackId) => {
      if (!line.trim()) return;
      const fields = line.trim().split(/\s+/);
      const last = fields[fields.length - 1];
      if (/^\d+$/.test(last)) {
        ans.set(Number(last), fields.slice(0, -1).join(" "));
      } else {
        ans.set(fallbackId, line.trim());
      }
    });
  return ans;
}

function decodeGreedy(filePath, vocabSize, tokens, layout, blankId = 0) {
  const buffer = fs.readFileSync(filePath);
  const total = buffer.length / 4;
  const frames = total / vocabSize;
  const ids = [];
  let prev = null;
  for (let frame = 0; frame < frames; frame += 1) {
    let bestId = -1;
    let bestValue = -Infinity;
    for (let token = 0; token < vocabSize; token += 1) {
      const index = layout === "frame_major"
        ? frame * vocabSize + token
        : token * frames + frame;
      const value = buffer.readFloatLE(index * 4);
      if (value > bestValue) {
        bestValue = value;
        bestId = token;
      }
    }
    if (bestId !== blankId && bestId !== prev) ids.push(bestId);
    prev = bestId;
  }
  return {
    layout,
    ids,
    text: ids.map((id) => tokens.get(id) || `<${id}>`).join(""),
  };
}

function floatLogitsStats(filePath, vocabSize, blankId = 0) {
  const buffer = fs.readFileSync(filePath);
  if (buffer.length % 4 !== 0) fail(`Float logits size is not divisible by 4: ${filePath}`);
  const total = buffer.length / 4;
  if (total % vocabSize !== 0) {
    fail(`Float logits count ${total} is not divisible by vocab size ${vocabSize}`);
  }

  let nonZero = 0;
  let nan = 0;
  let min = Infinity;
  let max = -Infinity;
  let sum = 0;
  for (let i = 0; i < total; i += 1) {
    const value = buffer.readFloatLE(i * 4);
    if (Number.isNaN(value)) {
      nan += 1;
      continue;
    }
    if (value !== 0) nonZero += 1;
    if (value < min) min = value;
    if (value > max) max = value;
    sum += value;
  }

  const frames = total / vocabSize;
  const greedyForLayout = (layout) => {
    let nonBlank = 0;
    const firstNonBlank = [];
    for (let frame = 0; frame < frames; frame += 1) {
      let bestId = -1;
      let bestValue = -Infinity;
      for (let token = 0; token < vocabSize; token += 1) {
        const index = layout === "frame_major"
          ? frame * vocabSize + token
          : token * frames + frame;
        const value = buffer.readFloatLE(index * 4);
        if (value > bestValue) {
          bestValue = value;
          bestId = token;
        }
      }
      if (bestId !== blankId) {
        nonBlank += 1;
        if (firstNonBlank.length < 20) firstNonBlank.push({ frame, token: bestId, value: bestValue });
      }
    }
    return { layout, blankId, nonBlank, frames, firstNonBlank };
  };

  return {
    bytes: buffer.length,
    floats: { total, nonZero, nan, min, max, mean: sum / (total - nan) },
    greedy: greedyForLayout("frame_major"),
    greedyByLayout: {
      frame_major: greedyForLayout("frame_major"),
      vocab_major: greedyForLayout("vocab_major"),
    },
  };
}

function writeConfigFiles(outDir, soc, arch, socId, graphName) {
  const parsedVtcmMb = Number(process.env.STT_QNN_VTCM_MB || 16);
  const vtcmMb = Number.isFinite(parsedVtcmMb) && parsedVtcmMb > 0 ? parsedVtcmMb : 16;
  fs.mkdirSync(outDir, { recursive: true });
  const htpBackendExtensions = {
    backend_extensions: {
      shared_library_path: "./libQnnHtpNetRunExtensions.so",
      config_file_path: "./htp_config.json",
    },
  };
  const htpConfig = {
    graphs: [
      {
        vtcm_mb: vtcmMb,
        O: 3,
        graph_names: [graphName],
      },
    ],
    devices: [
      {
        device_id: 0,
        soc_id: socId,
        dsp_arch: arch.toLowerCase(),
        cores: [
          {
            core_id: 0,
            perf_profile: "burst",
            rpc_control_latency: 200,
          },
        ],
      },
    ],
  };
  fs.writeFileSync(
    path.join(outDir, "htp_backend_extensions.json"),
    `${JSON.stringify(htpBackendExtensions, null, 2)}\n`,
  );
  fs.writeFileSync(path.join(outDir, "htp_config.json"), `${JSON.stringify(htpConfig, null, 2)}\n`);
}

function prepareInput0(input0, outDir) {
  const layout = (process.env.STT_QNN_INPUT0_LAYOUT || "feature_major").toLowerCase();
  if (!["feature_major", "onnx"].includes(layout)) {
    fail(`Unsupported STT_QNN_INPUT0_LAYOUT: ${layout}`);
  }
  if (layout === "feature_major") {
    return { input0, layout, originalInput0: input0 };
  }

  const frames = Number(process.env.STT_QNN_INPUT0_FRAMES || 167);
  const featureDim = Number(process.env.STT_QNN_INPUT0_FEATURE_DIM || 560);
  const buffer = fs.readFileSync(input0);
  const expectedBytes = frames * featureDim * 4;
  if (buffer.length !== expectedBytes) {
    fail(
      `ONNX-layout input0 has ${buffer.length} bytes, expected ${expectedBytes} ` +
        `for ${frames}x${featureDim}`,
    );
  }

  fs.mkdirSync(outDir, { recursive: true });
  const transposed = Buffer.allocUnsafe(buffer.length);
  for (let t = 0; t < frames; t += 1) {
    for (let d = 0; d < featureDim; d += 1) {
      const value = buffer.readFloatLE((t * featureDim + d) * 4);
      transposed.writeFloatLE(value, (d * frames + t) * 4);
    }
  }

  const preparedInput0 = path.join(outDir, "input0.feature_major.raw");
  fs.writeFileSync(preparedInput0, transposed);
  return { input0: preparedInput0, layout, originalInput0: input0 };
}

function main() {
  const soc = readDeviceSoc();
  const arch = inferHtpArch(soc);
  if (!arch) fail(`No HTP architecture mapping for ${soc}`);
  const socId = readSocId(soc);
  const backend = (process.env.STT_QNN_BACKEND || "htp").toLowerCase();
  if (!["htp", "cpu"].includes(backend)) {
    fail(`Unsupported STT_QNN_BACKEND: ${backend}`);
  }

  const modelOverride = process.env.STT_QNN_REFERENCE_MODEL;
  const graphName = process.env.STT_QNN_GRAPH_NAME || "model_10_seconds_quantized";
  const inputList = process.env.STT_QNN_INPUT_LIST || "x:=input0.float.raw prompt:=input1.int32.raw";
  const nativeInputTensorNames =
    process.env.STT_QNN_NATIVE_INPUT_TENSOR_NAMES === undefined
      ? `${graphName}:prompt`
      : process.env.STT_QNN_NATIVE_INPUT_TENSOR_NAMES;
  const nativeInputTensorNamesArg =
    nativeInputTensorNames.toLowerCase() === "none" ? "" : nativeInputTensorNames;
  const fixedModel = modelOverride || path.join(root, "build", "qnn-model-fixed", "libmodel.so");
  if (!modelOverride && !fs.existsSync(fixedModel)) {
    run(process.execPath, [
      path.join(root, "scripts", "repair_qnn_libmodel_elf.js"),
      path.join(root, "models", "sensevoice", "libmodel.so"),
      fixedModel,
    ]);
  }

  const input0 = requireFile(
    process.env.STT_QNN_INPUT0 ||
      path.join(root, "build", "test-results", "qnn-reference-input", "input0.raw"),
    "input0",
  );
  const input1 = requireFile(
    process.env.STT_QNN_INPUT1 ||
      path.join(root, "build", "test-results", "qnn-reference-input", "input1.raw"),
    "input1",
  );
  const localConfigDir = path.join(root, "build", "test-results", "qnn-net-run", "reference-gate-config");
  const localOutputDir = path.join(root, "build", "test-results", "qnn-net-run", "reference-gate-output");
  fs.rmSync(localOutputDir, { recursive: true, force: true });
  fs.mkdirSync(localOutputDir, { recursive: true });
  const preparedInput0 = prepareInput0(input0, localOutputDir);
  writeConfigFiles(localConfigDir, soc, arch, socId, graphName);

  const qnnLibDir = path.join(qnnSdk, "lib", "aarch64-android");
  const backendLibName = backend === "cpu" ? "libQnnCpu.so" : "libQnnHtp.so";
  const libcxxShared = path.join(
    root,
    "third_party",
    "android-ndk-r27c",
    "toolchains",
    "llvm",
    "prebuilt",
    "windows-x86_64",
    "sysroot",
    "usr",
    "lib",
    "aarch64-linux-android",
    "libc++_shared.so",
  );
  const htpArchs = ["V68", "V69", "V73", "V75", "V79", "V81"];
  const files = [
    [path.join(qnnSdk, "bin", "aarch64-android", "qnn-net-run"), "qnn-net-run"],
    [path.join(qnnLibDir, "libQnnHtp.so"), "libQnnHtp.so"],
    [path.join(qnnLibDir, "libQnnCpu.so"), "libQnnCpu.so"],
    [path.join(qnnLibDir, "libQnnHtpPrepare.so"), "libQnnHtpPrepare.so"],
    [path.join(qnnLibDir, "libQnnHtpNetRunExtensions.so"), "libQnnHtpNetRunExtensions.so"],
    [path.join(qnnLibDir, "libQnnSystem.so"), "libQnnSystem.so"],
    [libcxxShared, "libc++_shared.so"],
    [fixedModel, "libmodel.so"],
    [preparedInput0.input0, "input0.float.raw"],
    [input1, "input1.int32.raw"],
    [path.join(localConfigDir, "htp_backend_extensions.json"), "htp_backend_extensions.json"],
    [path.join(localConfigDir, "htp_config.json"), "htp_config.json"],
  ];
  for (const htpArch of htpArchs) {
    const skelDir = path.join(qnnSdk, "lib", `hexagon-${htpArch.toLowerCase()}`, "unsigned");
    files.push(
      [path.join(qnnLibDir, `libQnnHtp${htpArch}Stub.so`), `libQnnHtp${htpArch}Stub.so`],
      [
        path.join(qnnLibDir, `libQnnHtp${htpArch}CalculatorStub.so`),
        `libQnnHtp${htpArch}CalculatorStub.so`,
      ],
      [path.join(skelDir, `libQnnHtp${htpArch}Skel.so`), `libQnnHtp${htpArch}Skel.so`],
    );
  }

  run(adb, ["shell", `rm -rf ${sandbox} && mkdir -p ${sandbox}`]);
  for (const [local, remoteName] of files) push(requireFile(local), remoteName);
  run(adb, ["shell", `chmod +x ${sandbox}/qnn-net-run`]);
  run(adb, [
    "shell",
    [
      `cd ${sandbox}`,
      "rm -rf output",
      `printf '${inputList}\\n' > input_list_reference.txt`,
      [
        "LD_LIBRARY_PATH=. ./qnn-net-run",
        "--model ./libmodel.so",
        `--backend ./${backendLibName}`,
      backend === "htp" ? "--config_file ./htp_backend_extensions.json" : "",
      "--input_list ./input_list_reference.txt",
      "--output_dir ./output/reference",
      nativeInputTensorNamesArg ? `--native_input_tensor_names ${nativeInputTensorNamesArg}` : "",
      "--log_level info",
    ].filter(Boolean).join(" "),
  ].join(" && "),
  ]);

  run(adb, [
    "pull",
    `${sandbox}/output/reference/Result_0/logits.raw`,
    path.join(localOutputDir, "logits.raw"),
  ]);

  const vocabSize = countTokens();
  const tokens = loadTokens();
  const logitsPath = path.join(localOutputDir, "logits.raw");
  const result = {
    deviceSoc: soc,
    htpArch: arch,
    socId,
    backend,
    vocabSize,
    output: floatLogitsStats(logitsPath, vocabSize),
    decoded: {
      frame_major: decodeGreedy(logitsPath, vocabSize, tokens, "frame_major"),
      vocab_major: decodeGreedy(logitsPath, vocabSize, tokens, "vocab_major"),
    },
    inputFiles: {
      input0: preparedInput0.input0,
      input0Layout: preparedInput0.layout,
      originalInput0: preparedInput0.originalInput0,
      input1,
    },
    localOutputDir,
  };
  console.log(JSON.stringify(result, null, 2));

  if (!reportOnly && result.output.greedyByLayout.frame_major.nonBlank === 0) {
    fail("qnn-net-run reference gate failed: frame-major float logits greedy output is all blank");
  }
  const expectedContains = process.env.STT_QNN_EXPECT_CONTAINS;
  if (
    !reportOnly &&
    expectedContains &&
    !result.decoded.frame_major.text.includes(expectedContains)
  ) {
    fail(
      `qnn-net-run reference gate failed: decoded text missing ${JSON.stringify(expectedContains)}`,
    );
  }
}

main();
