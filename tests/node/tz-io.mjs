// "tz:io" for the tests that run under Node too (see preload.cjs): readLine
// as tz has it (docs/io.md, section 4), reading stdin synchronously.
import { readSync } from "node:fs";

let pending = Buffer.alloc(0);
let ended = false;

// Reads more of stdin; false at its end.
function fill() {
  if (ended)
    return false;
  const chunk = Buffer.alloc(65536);
  let n;
  for (;;) {
    try {
      n = readSync(0, chunk, 0, chunk.length, null);
      break;
    } catch (e) {
      if (e.code === "EAGAIN")
        continue;
      if (e.code === "EOF") {
        n = 0;
        break;
      }
      throw e;
    }
  }
  if (n === 0) {
    ended = true;
    return false;
  }
  pending = Buffer.concat([pending, chunk.subarray(0, n)]);
  return true;
}

// The question as it is, then a line without its "\n" or "\r\n";
// undefined at the end of the input.
export function readLine(question) {
  if (question !== undefined)
    process.stdout.write(question);
  for (let scanned = 0;;) {
    const i = pending.indexOf(10, scanned);
    if (i >= 0) {
      const end = i > 0 && pending[i - 1] === 13 ? i - 1 : i;
      const line = pending.toString("utf8", 0, end);
      pending = pending.subarray(i + 1);
      return line;
    }
    scanned = pending.length;
    if (!fill()) {
      if (pending.length === 0)
        return undefined;
      const line = pending.toString("utf8");
      pending = Buffer.alloc(0);
      return line;
    }
  }
}
