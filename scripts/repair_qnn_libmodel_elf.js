const fs = require("fs");
const path = require("path");

function fail(message) {
  console.error(`[Error] ${message}`);
  process.exit(1);
}

function align(value, boundary) {
  return Math.ceil(value / boundary) * boundary;
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
      offset: buffer.readBigUInt64LE(offset + 8),
      vaddr: buffer.readBigUInt64LE(offset + 16),
      filesz: buffer.readBigUInt64LE(offset + 32),
    });
  }

  return headers;
}

function vaddrToOffset(loads, vaddr) {
  for (const load of loads) {
    const start = load.vaddr;
    const end = load.vaddr + load.filesz;
    if (vaddr >= start && vaddr < end) {
      return load.offset + (vaddr - start);
    }
  }
  fail(`Cannot map virtual address 0x${vaddr.toString(16)} to a file offset`);
}

function readDynamicValues(buffer, dynamicOffset, dynamicSize) {
  let strtab = null;
  let strsz = null;
  const count = Number(dynamicSize / 16n);

  for (let i = 0; i < count; i += 1) {
    const offset = Number(dynamicOffset) + i * 16;
    const tag = buffer.readBigInt64LE(offset);
    const value = buffer.readBigUInt64LE(offset + 8);
    if (tag === 0n) break;
    if (tag === 5n) strtab = value;
    if (tag === 10n) strsz = value;
  }

  if (strtab === null) fail("DT_STRTAB was not found");
  if (strsz === null) fail("DT_STRSZ was not found");
  return { strtab, strsz };
}

function writeSectionHeader(buffer, index, shoff, fields) {
  const offset = shoff + index * 64;
  buffer.writeUInt32LE(fields.name || 0, offset);
  buffer.writeUInt32LE(fields.type || 0, offset + 4);
  buffer.writeBigUInt64LE(fields.flags || 0n, offset + 8);
  buffer.writeBigUInt64LE(fields.addr || 0n, offset + 16);
  buffer.writeBigUInt64LE(fields.offset || 0n, offset + 24);
  buffer.writeBigUInt64LE(fields.size || 0n, offset + 32);
  buffer.writeUInt32LE(fields.link || 0, offset + 40);
  buffer.writeUInt32LE(fields.info || 0, offset + 44);
  buffer.writeBigUInt64LE(fields.addralign || 0n, offset + 48);
  buffer.writeBigUInt64LE(fields.entsize || 0n, offset + 56);
}

function repair(input, output) {
  const source = fs.readFileSync(input);
  if (source.subarray(0, 4).toString("hex") !== "7f454c46") fail("Input is not an ELF file");
  if (source[4] !== 2 || source[5] !== 1) fail("Expected a 64-bit little-endian ELF file");

  const programHeaders = readProgramHeaders(source);
  const loads = programHeaders.filter((header) => header.type === 1);
  const dynamic = programHeaders.find((header) => header.type === 2);
  if (!dynamic) fail("PT_DYNAMIC was not found");

  const { strtab, strsz } = readDynamicValues(source, dynamic.offset, dynamic.filesz);
  const dynstrOffset = vaddrToOffset(loads, strtab);

  const shstrtab = Buffer.from("\0.dynamic\0.dynstr\0.shstrtab\0");
  const shstrtabOffset = align(source.length, 8);
  const sectionHeaderOffset = align(shstrtabOffset + shstrtab.length, 8);
  const out = Buffer.alloc(sectionHeaderOffset + 64 * 4);
  source.copy(out, 0);
  shstrtab.copy(out, shstrtabOffset);

  writeSectionHeader(out, 1, sectionHeaderOffset, {
    name: 1,
    type: 6,
    flags: 3n,
    addr: dynamic.vaddr,
    offset: dynamic.offset,
    size: dynamic.filesz,
    link: 2,
    addralign: 8n,
    entsize: 16n,
  });
  writeSectionHeader(out, 2, sectionHeaderOffset, {
    name: 10,
    type: 3,
    flags: 2n,
    addr: strtab,
    offset: dynstrOffset,
    size: strsz,
    addralign: 1n,
  });
  writeSectionHeader(out, 3, sectionHeaderOffset, {
    name: 18,
    type: 3,
    offset: BigInt(shstrtabOffset),
    size: BigInt(shstrtab.length),
    addralign: 1n,
  });

  out.writeBigUInt64LE(BigInt(sectionHeaderOffset), 0x28);
  out.writeUInt16LE(64, 0x3a);
  out.writeUInt16LE(4, 0x3c);
  out.writeUInt16LE(3, 0x3e);

  fs.mkdirSync(path.dirname(output), { recursive: true });
  fs.writeFileSync(output, out);
}

const [input, output] = process.argv.slice(2);
if (!input || !output) fail("Usage: node scripts/repair_qnn_libmodel_elf.js <input.so> <output.so>");
repair(input, output);
