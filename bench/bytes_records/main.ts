// 4 million records of 16 bytes written and read back through a DataView:
// a little-endian uint32 id, a float64 value, and two big-endian uint16;
// then the same reads through Buffer's readUInt32LE and readDoubleLE.
const count = 4 * 1024 * 1024
const view = new DataView(new ArrayBuffer(count * 16))
let seed = 1
for (let off = 0; off + 16 <= view.byteLength; off += 16) {
  seed = (Math.imul(seed, 1103515245) + 12345) | 0
  view.setUint32(off, seed >>> 0, true)
  view.setFloat64(off + 4, (seed & 0xffff) / 64, true)
  view.setUint16(off + 12, seed & 0xffff)
  view.setUint16(off + 14, (seed >>> 16) & 0xffff)
}
let ids: long = 0
let values: double = 0
let small: long = 0
for (let round = 0; round < 5; round++) {
  for (let off = 0; off + 16 <= view.byteLength; off += 16) {
    ids += view.getUint32(off, true)
    values += view.getFloat64(off + 4, true)
    small += view.getUint16(off + 12) ^ view.getUint16(off + 14)
  }
}
const buf = Buffer.from(view.buffer)
let check: long = 0
let total: double = 0
for (let off = 0; off + 16 <= buf.length; off += 16) {
  check += buf.readUInt32LE(off)
  total += buf.readDoubleLE(off + 4)
}
console.log(ids, values, small, check, total)
