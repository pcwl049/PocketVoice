#!/usr/bin/env node

// Character-level CER between a hypothesis and a reference transcript.
// Whitespace and common CJK/ASCII punctuation are stripped from both sides
// before comparison (SenseVoice may emit trailing punctuation the reference
// does not contain).
//
// Usage: node scripts/text_cer.js "<hypothesis>" "<reference>"
// Prints: <edit-distance>\t<ref-length>\t<cer>

function normalize(text) {
  return String(text || "")
    .replace(/[\s。，？！、：；（）「」『』《》…—·,.?!:;()"'`\[\]{}]/g, "");
}

function levenshtein(a, b) {
  if (a === b) return 0;
  if (a.length === 0) return b.length;
  if (b.length === 0) return a.length;
  let prev = new Array(b.length + 1);
  for (let j = 0; j <= b.length; j += 1) prev[j] = j;
  for (let i = 1; i <= a.length; i += 1) {
    const curr = [i];
    for (let j = 1; j <= b.length; j += 1) {
      const cost = a[i - 1] === b[j - 1] ? 0 : 1;
      curr[j] = Math.min(prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost);
    }
    prev = curr;
  }
  return prev[b.length];
}

const [, , rawHyp, rawRef] = process.argv;
if (rawRef === undefined) {
  console.error("Usage: node scripts/text_cer.js \"<hypothesis>\" \"<reference>\"");
  process.exit(1);
}

const hyp = normalize(rawHyp);
const ref = normalize(rawRef);
const distance = levenshtein(hyp, ref);
const cer = ref.length > 0 ? distance / ref.length : (hyp.length > 0 ? 1 : 0);
console.log(`${distance}\t${ref.length}\t${cer.toFixed(4)}`);
