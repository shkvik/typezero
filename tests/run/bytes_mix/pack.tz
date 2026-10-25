// A binary format of records: a header, then for each record a 16-bit id,
// a 32-bit count and a double, little-endian, then the name as bytes.

export class Record {
  constructor(id: int, count: long, price: number, name: string) {
    this.id = id
    this.count = count
    this.price = price
    this.name = name
  }
  id: int
  count: long
  price: number
  name: string
}

const MAGIC = new Uint8Array([0x54, 0x5a, 0x42, 0x31]) // "TZB1"

export function encode(records: Record[]): Uint8Array {
  const names = records.map(r => new TextEncoder().encode(r.name))
  let size = 8
  for (const n of names)
    size += 15 + n.length
  const out = new Uint8Array(size)
  out.set(MAGIC, 0)
  const view = new DataView(out.buffer)
  view.setUint32(4, records.length, true)
  let at = 8
  records.forEach((r, i) => {
    const name = names[i] ?? new Uint8Array(0)
    view.setUint16(at, r.id & 0xffff, true)
    view.setUint32(at + 2, Math.min(Math.max(r.count, 0), 4294967295), true)
    view.setFloat64(at + 6, r.price, true)
    view.setUint8(at + 14, name.length & 0xff)
    out.set(name, at + 15)
    at += 15 + name.length
  })
  return out
}

export function decode(bytes: Uint8Array): Record[] {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
  for (let i = 0; i < MAGIC.length; i++)
    if (view.getUint8(i) != MAGIC[i])
      throw new Error("not a TZB1 file")
  const count = view.getUint32(4, true)
  const records: Record[] = []
  const decoder = new TextDecoder()
  let at = 8
  for (let i = 0; i < count; i++) {
    const length = view.getUint8(at + 14)
    const name = decoder.decode(bytes.subarray(at + 15, at + 15 + length))
    records.push(new Record(view.getUint16(at, true), view.getUint32(at + 2, true),
                            view.getFloat64(at + 6, true), name))
    at += 15 + length
  }
  return records
}

// The bytes of text or of bytes, as one kind.
export function bytesOf(data: Uint8Array | string | ArrayBuffer): Uint8Array {
  if (typeof data == "string")
    return new TextEncoder().encode(data)
  if (data instanceof ArrayBuffer)
    return new Uint8Array(data)
  return data
}

export function describe(data: Uint8Array | string | ArrayBuffer | Float64Array): string {
  if (Buffer.isBuffer(data))
    return `Buffer of ${data.length}: ${data.toString("hex")}`
  if (data instanceof Uint8Array)
    return `Uint8Array of ${data.length}`
  if (data instanceof Float64Array)
    return `Float64Array, first ${data[0] ?? 0}`
  if (data instanceof ArrayBuffer)
    return `ArrayBuffer of ${data.byteLength}`
  return `text "${data}"`
}

// A 32-bit FNV-1a hash, as unsigned.
export function fnv1a(bytes: Uint8Array) {
  let h = 0x811c9dc5 | 0
  for (const b of bytes)
    h = Math.imul(h ^ b, 0x01000193)
  return h >>> 0
}
