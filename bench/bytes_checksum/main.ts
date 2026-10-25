// 100 MB of bytes: filled, XORed with a 16-byte key, summed, and hashed with
// 32-bit FNV-1a; each a loop over a Uint8Array.
function fill(data: Uint8Array) {
  for (let i = 0; i < data.length; i++) data[i] = (i * 7 + (i >>> 9)) & 0xff
}

function xor(data: Uint8Array, key: Uint8Array) {
  if (key.length < 16) return
  for (let i = 0; i < data.length; i++) data[i] ^= key[i & 15]
}

function sum(data: Uint8Array): long {
  let s: long = 0
  for (let i = 0; i < data.length; i++) s += data[i]
  return s
}

function fnv1a(data: Uint8Array): long {
  let h = 0x811c9dc5 | 0
  for (let i = 0; i < data.length; i++) h = Math.imul(h ^ data[i], 0x01000193)
  return h >>> 0
}

const data = new Uint8Array(100 * 1024 * 1024)
const key = new Uint8Array([0x13, 0x37, 0xc0, 0xde, 0xba, 0xad, 0xf0, 0x0d,
                            0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef])
fill(data)
const before = sum(data)
xor(data, key)
const encrypted = fnv1a(data)
xor(data, key)
console.log(before, encrypted, sum(data), fnv1a(data))
