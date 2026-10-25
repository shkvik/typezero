// same-as-node
// Typed arrays in fields, arrays, optionals and generic code, iterated,
// with callbacks that throw, and the RangeErrors of JavaScript.
class Image {
  pixels: Uint8ClampedArray
  width: int
  constructor(width: int, height: int) {
    this.width = width
    this.pixels = new Uint8ClampedArray(width * height)
  }
  brighten(by: int) {
    const p = this.pixels
    for (let i = 0; i < p.length; i++) p[i] = p[i] + by
  }
}
const img = new Image(2, 2)
img.brighten(100)
img.brighten(200)
console.log(img, img.pixels)
function first<T>(xs: T[]): T | undefined {
  return xs[0]
}
const list: Uint8Array[] = [new Uint8Array([1]), new Uint8Array([2, 3])]
console.log(first(list), list)
let maybe: Uint8Array | undefined = undefined
console.log(maybe?.length, maybe)
maybe = list[1]
if (maybe) console.log(maybe.length, maybe[0] ?? -1)
const make = (n: int) => new Float32Array(n).fill(0.5)
console.log(make(3))
const bytes = new Uint8Array([10, 20, 30])
for (const [i, b] of bytes.entries()) console.log(i, b)
console.log([...bytes.keys()], [...bytes.values()].map(x => x * 2))
let total = 0
for (const b of bytes) {
  if (b > 20) break
  total += b
}
console.log(total)
const mixed = [...bytes, 4, ...new Int8Array([-1])]
console.log(mixed)
function* gen() {
  yield 1
  yield 2
}
console.log(new Int32Array(gen()), Uint8Array.from(gen(), x => (x * 3) & 0xff))
try {
  bytes.forEach(x => {
    if (x > 15) throw new Error(`too big: ${x}`)
  })
} catch (e) {
  console.log(e.message)
}
const n = -1
try {
  const bad = new Uint8Array(n + bytes.length - 4)
  console.log(bad)
} catch (e) {
  console.log(e.name, e.message)
}
const view = new DataView(bytes.buffer)
try {
  console.log(view.getUint16(2))
} catch (e) {
  console.log(e.name, e.message)
}
const buf = Buffer.from("abc")
try {
  console.log(buf.readUInt32LE(0))
} catch (e) {
  console.log(e.name, e.message)
}
console.log(buf.length, buf.byteLength, buf.byteOffset >= 0, buf.buffer.byteLength >= 3)
const sorted = new Int32Array([3, -1, 2]).sort((a, b) => a - b)
console.log(sorted, new Float64Array([0.5, -2]).reduce((a, b) => a + b, 10))
console.log(bytes == bytes, bytes == new Uint8Array([10, 20, 30]), typeof bytes)
