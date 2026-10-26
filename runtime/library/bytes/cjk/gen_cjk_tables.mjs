import fs from "node:fs";
import path from "node:path";

const dir = process.argv[2];
if (!dir) {
  console.error("usage: node gen_cjk_tables.mjs <directory of index-*.txt>");
  process.exit(1);
}

function read(name) {
  const text = fs.readFileSync(path.join(dir, `index-${name}.txt`), "utf8");
  const entries = new Map();
  for (const line of text.split("\n")) {
    if (!line.trim() || line.startsWith("#")) continue;
    const [pointer, codePoint] = line.trim().split(/\s+/);
    entries.set(Number(pointer), Number(codePoint));
  }
  return { name, entries };
}

const indexes = ["gb18030", "gb18030-ranges", "big5", "jis0208", "jis0212", "euc-kr"].map(read);
const byName = new Map(indexes.map((i) => [i.name, i]));
const hex = (n, width = 4) => "0x" + n.toString(16).toUpperCase().padStart(width, "0");
const lines = [];

function array(type, cName, values, width) {
  lines.push(`static const ${type} ${cName}[${values.length}] = {`);
  const perLine = width > 4 ? 6 : 12;
  for (let i = 0; i < values.length; i += perLine)
    lines.push("    " + values.slice(i, i + perLine).map((v) => hex(v, width)).join(", ") + ",");
  lines.push("};");
  lines.push("");
}

function dense(index, cName, macro, { astral = false } = {}) {
  const pointers = [...index.entries.keys()];
  const first = Math.min(...pointers), last = Math.max(...pointers);
  const values = [];
  const bits = new Array(Math.ceil((last - first + 1) / 8)).fill(0);
  for (let p = first; p <= last; p++) {
    const cp = index.entries.get(p) ?? 0;
    if (cp > 0xffff) {
      if (!astral || cp >> 16 != 2)
        throw new Error(`${index.name}: pointer ${p} is U+${cp.toString(16)}`);
      bits[(p - first) >> 3] |= 1 << ((p - first) & 7);
    }
    if (cp != 0 && (cp & 0xffff) == 0)
      throw new Error(`${index.name}: pointer ${p} would read as none`);
    values.push(cp & 0xffff);
  }
  lines.push(`#define ${macro}_FIRST ${first}`);
  lines.push(`#define ${macro}_COUNT ${values.length}`);
  array("uint16_t", cName, values, 4);
  if (astral)
    array("uint8_t", cName + "_astral", bits, 2);
}

lines.push("// Copyright © WHATWG (Apple, Google, Mozilla, Microsoft). The Encoding Standard is");
lines.push("// licensed under a Creative Commons Attribution 4.0 International License; to the");
lines.push("// extent portions of it are incorporated into source code, such portions in the");
lines.push("// source code are licensed under the BSD 3-Clause License instead (see");
lines.push("// third_party/licenses/WHATWG.txt).");
lines.push("");
lines.push("#ifndef TZ_CJK_TABLES_H");
lines.push("#define TZ_CJK_TABLES_H");
lines.push("");
lines.push("#include <stdint.h>");
lines.push("");
dense(byName.get("gb18030"), "tz_cjk_gb18030", "TZ_CJK_GB18030");
const ranges = [...byName.get("gb18030-ranges").entries].sort((a, b) => a[0] - b[0]);
lines.push(`#define TZ_CJK_GB18030_RANGES ${ranges.length}`);
array("uint32_t", "tz_cjk_gb18030_ranges", ranges.flat(), 6);
dense(byName.get("big5"), "tz_cjk_big5", "TZ_CJK_BIG5", { astral: true });
dense(byName.get("jis0208"), "tz_cjk_jis0208", "TZ_CJK_JIS0208");
dense(byName.get("jis0212"), "tz_cjk_jis0212", "TZ_CJK_JIS0212");
dense(byName.get("euc-kr"), "tz_cjk_euc_kr", "TZ_CJK_EUC_KR");
lines.push("#endif");
console.log(lines.join("\n"));
