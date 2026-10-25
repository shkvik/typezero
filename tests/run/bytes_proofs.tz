// same-as-node
// What proves that a value fits in an element (docs/bytes.md, section 3):
// literals, masks, checks above, elements of a narrower kind, Math.min and
// Math.max, the element a callback gets; and the proofs of indices, which
// outlive calls since a typed array keeps its length.
function checksum(data: Uint8Array): int {
  let sum = 0
  for (let i = 0; i < data.length; i++) sum = (sum + data[i]) & 0xffff
  return sum
}

function noise(seed: int, n: int): Uint8Array {
  const out = new Uint8Array(n)
  let x = seed
  for (let i = 0; i < out.length; i++) {
    x = (Math.imul(x, 1103515245) + 12345) | 0
    out[i] = x >>> 24
  }
  return out
}

function clampTo(x: int, out: Uint8Array) {
  if (out.length > 0) {
    if (x >= 0 && x <= 255) out[0] = x
    else out[0] = Math.min(Math.max(x, 0), 255)
  }
}

const bytes = noise(42, 16)
console.log(bytes, checksum(bytes))
const copy = new Uint8Array(bytes.length)
for (let i = 0; i < bytes.length; i++) {
  console.log(checksum(copy)) // a call: the lengths are still known
  copy[i] = bytes[i]
}
console.log(copy)
const small = new Uint8Array(2)
clampTo(300, small)
console.log(small)
clampTo(-5, small)
console.log(small)

// Compound assignments that stay in the element.
const flags = new Uint8Array([0x0f, 0xf0])
flags[0] ^= 0xff
flags[1] &= 0x3c
flags[0] >>= 2
flags[1] |= 1
console.log(flags)

// Elements of narrower kinds, and the range a callback's element has.
const wide = new Int16Array(4)
const signed = new Int8Array([-128, -1, 0, 127])
for (let i = 0; i < signed.length && i < wide.length; i++) wide[i] = signed[i]
console.log(wide, signed.map(x => -1 - x), bytes.map(x => 255 - x).subarray(0, 4))
const halves = new Uint16Array(bytes.length)
for (let i = 0; i < bytes.length; i++) halves[i] = (bytes[i] << 8) | bytes[i]
console.log(halves.subarray(0, 4), Uint8Array.from(halves, h => h >> 8).subarray(0, 4))

// Uint32 elements are longs; `>>> 0` fits them.
const words = new Uint32Array(2)
const all = -1 >>> 0
words[0] = all
words[1] = (words[0] & 0xffff) * 2
console.log(words, words[0] + words[1])

// A loop to Math.min of two lengths.
function same(a: Uint8Array, b: Uint8Array): boolean {
  const n = Math.min(a.length, b.length)
  for (let i = 0; i < n; i++) if (a[i] != b[i]) return false
  return a.length == b.length
}
console.log(same(bytes, copy), same(bytes, small))
