// same-as-node
// The errors JavaScript throws when the program runs, with node's messages:
// lengths and offsets known only then, reads out of range, unknown
// encodings, bytes a fatal TextDecoder cannot decode.
function attempt(what: string, f: () => void) {
  try {
    f()
    console.log(what, "ok")
  } catch (e) {
    console.log(what, e.name, e.message)
  }
}
const minus = [-1, 1][0]
const sizes = [3, 4, 5, 7]
const buffer = new ArrayBuffer(sizes[1] ?? 0)
const odd = new ArrayBuffer(sizes[0] ?? 0)
attempt("length", () => console.log(new Uint8Array(minus)))
attempt("buffer", () => console.log(new ArrayBuffer(minus)))
attempt("align", () => console.log(new Uint16Array(buffer, -minus)))
attempt("bytes", () => console.log(new Uint16Array(odd)))
attempt("start", () => console.log(new Uint8Array(odd, sizes[1] ?? 0)))
attempt("end", () => console.log(new Uint8Array(odd, 1, sizes[2] ?? 0)))
attempt("view start", () => console.log(new DataView(odd, sizes[1] ?? 0)))
attempt("view end", () => console.log(new DataView(odd, 1, sizes[2] ?? 0)))
attempt("get", () => console.log(new DataView(odd).getUint32(0)))
attempt("set", () => new Uint8Array(3).set([1, 2, 3], -minus))
attempt("with", () => console.log(new Uint8Array(3).with(sizes[0] ?? 0, 1)))
attempt("reduce", () => console.log(new Uint8Array(0).reduce((a, b) => a + b)))
const four = Buffer.alloc(4)
try {
  console.log(Buffer.alloc(minus))
} catch (e) {
  // The largest size differs: 2^53 - 1 bytes in node, 2^30 - 1 here.
  console.log("alloc", e.name, e.message.startsWith('The value of "size" is out of range'))
}
attempt("from start", () => console.log(Buffer.from(buffer, sizes[2] ?? 0)))
attempt("from end", () => console.log(Buffer.from(buffer, 1, sizes[2] ?? 0)))
attempt("read", () => console.log(four.readUInt8(sizes[1] ?? 0)))
attempt("read 16", () => console.log(four.readUInt16LE(sizes[0] ?? 0)))
attempt("read short", () => console.log(Buffer.alloc(1).readUInt16LE(0)))
attempt("read minus", () => console.log(four.readUInt8(minus)))
attempt("write", () => console.log(four.writeUInt8(1, sizes[1] ?? 0)))
attempt("write 32", () => console.log(four.writeUInt32LE(1, 1)))
attempt("write text", () => console.log(four.write("ab", sizes[2] ?? 0)))
attempt("read var", () => console.log(four.readUIntLE(0, sizes[3] ?? 0)))
attempt("read var end", () => console.log(four.readUIntLE(1, sizes[1] ?? 0)))
attempt("swap", () => console.log(Buffer.alloc(3).swap16()))
attempt("copy", () => console.log(four.copy(Buffer.alloc(2), minus)))
attempt("copy source", () => console.log(four.copy(Buffer.alloc(2), 0, sizes[2] ?? 0)))
const encodings = ["hex", "nope", "x-mac-cyrillic", "utf-17"]
attempt("encoding", () => console.log(four.toString(encodings[1] ?? "")))
attempt("fill", () => console.log(four.fill("zz", "hex")))
attempt("label", () => console.log(new TextDecoder(encodings[3] ?? "")))
attempt("label old", () => console.log(new TextDecoder(encodings[2] ?? "").decode(new Uint8Array([0xc0, 0x80]))))
attempt("fatal", () => console.log(new TextDecoder("utf-8", { fatal: true }).decode(new Uint8Array([0x41, 0xff]))))
attempt("fatal 16", () => console.log(new TextDecoder("utf-16le", { fatal: true }).decode(new Uint8Array([0, 0xd8]))))
attempt("fatal 1253", () => console.log(new TextDecoder("windows-1253", { fatal: true }).decode(new Uint8Array([0xaa]))))
