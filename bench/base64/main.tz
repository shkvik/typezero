// 30 MB of bytes to base64 and back with Buffer, five times, and the same
// bytes as hex once.
const data = new Uint8Array(30 * 1024 * 1024)
let x = 12345
for (let i = 0; i < data.length; i++) {
  x = (Math.imul(x, 1103515245) + 12345) | 0
  data[i] = x >>> 24
}
const bytes = Buffer.from(data)
let length = 0
let same = true
for (let round = 0; round < 5; round++) {
  const text = bytes.toString("base64")
  length += text.length
  const back = Buffer.from(text, "base64")
  same = same && back.equals(bytes)
}
const hex = bytes.toString("hex")
console.log(length, same, hex.length, hex.slice(0, 16), Buffer.from(hex, "hex").equals(bytes))
