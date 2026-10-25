// A package from a directory that uses node:path, node:os and node:buffer,
// Buffer, async functions, compiled regexes, a generic class and unions; and
// its files import each other.
import { Counter, decode, describe, encode, fileKind, lines, slowJoin, slugOf, words } from "textkit"

console.log(fileKind("docs/readme.md"), fileKind("Makefile"))
const b64 = encode("Привет, tz!")
console.log(b64, decode(b64))
console.log(lines("a", "b").split("\n").length)
const counter = new Counter<string>()
for (const w of words("one two, one: Three two one"))
  counter.add(w.toLowerCase())
console.log(counter.counts)
console.log(await slowJoin("a", "b.txt"))
console.log(slugOf("Hello, World — tz 2026!"), describe(1), describe("s"), describe(Buffer.from("hi")))
