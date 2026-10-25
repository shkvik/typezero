// same-as-node
// A property every member of a union has, read without narrowing: of binary
// data, of plain objects and of classes.

type Data = Uint8Array | DataView | Buffer | ArrayBuffer
function size(d: Data) {
  return d.byteLength
}
const memory = new ArrayBuffer(16)
const all: Data[] = [new Uint8Array(3), new DataView(memory, 4), Buffer.from("héllo"), memory,
  new Uint8Array(memory, 2, 5)]
console.log(all.map(size))

// Elements of other sizes: the byte length by the member it is.
type Numbers = Uint8Array | Int16Array | Float64Array | Uint32Array
function bytes(n: Numbers) {
  return `${n.length} elements, ${n.byteLength} bytes from ${n.byteOffset}`
}
console.log(bytes(new Uint8Array(4)), "|", bytes(new Int16Array(4)))
console.log(bytes(new Float64Array(memory, 8)), "|", bytes(new Uint32Array(memory, 4, 2)))

type View = Uint8Array | DataView | Int32Array
function where(v: View) {
  const whole = v.buffer
  return [v.byteOffset, v.byteLength, whole.byteLength, whole == memory ? 1 : 0]
}
console.log(where(new DataView(memory, 3, 4)), where(new Int32Array(memory, 8)),
  where(new Uint8Array(2)))

// Plain objects and classes: one field read through the union, and written.
interface Circle { kind: "circle"; name: string; r: number }
interface Square { kind: "square"; name: string; side: number }
function label(s: Circle | Square) {
  return `${s.kind} ${s.name}`
}
const c: Circle = { kind: "circle", name: "c", r: 1 }
const q: Square = { kind: "square", name: "q", side: 2 }
console.log(label(c), label(q))

class Cat {
  name: string
  lives = 9
  constructor(name: string) {
    this.name = name
  }
}
class Robot {
  serial = 7
  name: string
  constructor(name: string) {
    this.name = name
  }
}
function rename(x: Cat | Robot, to: string) {
  const old = x.name
  x.name = to
  return `${old} -> ${x.name}`
}
console.log(rename(new Robot("r2"), "r3"), rename(new Cat("tom"), "jerry"))
