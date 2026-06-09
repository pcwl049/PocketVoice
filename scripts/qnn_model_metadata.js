#!/usr/bin/env node

const fs = require("fs");
const path = require("path");

function fail(message) {
  console.error(`[Error] ${message}`);
  process.exit(1);
}

function tensorSummary(tensors, name) {
  const tensor = tensors[name];
  if (!tensor) return null;
  const scaleOffset = tensor.quant_params && tensor.quant_params.scale_offset;
  return {
    name,
    dims: tensor.dims,
    data_type: tensor.data_type,
    unquantized_data_type: tensor.unquantized_data_type,
    permute_order_to_src: tensor.permute_order_to_src,
    quant: scaleOffset
      ? {
          bitwidth: scaleOffset.bitwidth,
          minimum: scaleOffset.minimum,
          maximum: scaleOffset.maximum,
          scale: scaleOffset.scale,
          offset: scaleOffset.offset,
          is_symmetric: scaleOffset.is_symmetric,
        }
      : null,
  };
}

function commandField(command, name) {
  const match = command.match(new RegExp(`${name}=([^;]*)`));
  return match ? match[1].trim() : null;
}

function summarize(modelNetJson) {
  const report = JSON.parse(fs.readFileSync(modelNetJson, "utf8"));
  const graph = report.graph;
  if (!graph || !graph.tensors) fail(`Missing graph.tensors in ${modelNetJson}`);

  const converterCommand = report.converter_command || "";
  const tensors = graph.tensors;
  const ctcTensors = Object.keys(tensors)
    .filter((name) => /ctc_lo|logits/.test(name))
    .sort()
    .map((name) => tensorSummary(tensors, name));

  return {
    modelNetJson: path.resolve(modelNetJson),
    converter: {
      act_bitwidth: commandField(converterCommand, "act_bitwidth"),
      weights_bitwidth: commandField(converterCommand, "weights_bitwidth"),
      bias_bitwidth: commandField(converterCommand, "bias_bitwidth"),
      act_quantizer_calibration: commandField(converterCommand, "act_quantizer_calibration"),
      param_quantizer_calibration: commandField(converterCommand, "param_quantizer_calibration"),
      input_list: commandField(converterCommand, "input_list"),
      out_names: commandField(converterCommand, "out_names"),
      preserve_io: commandField(converterCommand, "preserve_io"),
      use_per_channel_quantization: commandField(converterCommand, "use_per_channel_quantization"),
      use_per_row_quantization: commandField(converterCommand, "use_per_row_quantization"),
      restrict_quantization_steps: commandField(converterCommand, "restrict_quantization_steps"),
    },
    io: {
      x: tensorSummary(tensors, "x"),
      logits: tensorSummary(tensors, "logits"),
    },
    ctcTensors,
  };
}

function main() {
  const args = process.argv.slice(2);
  const modelNetJson = args[0];
  if (!modelNetJson) {
    fail("Usage: node scripts/qnn_model_metadata.js <model_net.json>");
  }
  if (!fs.existsSync(modelNetJson)) fail(`Missing model_net.json: ${modelNetJson}`);
  console.log(JSON.stringify(summarize(modelNetJson), null, 2));
}

if (require.main === module) main();

module.exports = { summarize };
