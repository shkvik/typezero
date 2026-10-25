// A property of a union of binary data that some member lacks.
function offset(d: Uint8Array | ArrayBuffer) {
  // expect-error: property 'byteOffset' does not exist on 'ArrayBuffer', which 'Uint8Array | ArrayBuffer' may be; narrow it first, e.g. 'if (x instanceof Uint8Array)'
  return d.byteOffset
}
function count(v: Uint8Array | DataView) {
  // expect-error: property 'length' does not exist on 'DataView', which 'Uint8Array | DataView' may be; narrow it first
  return v.length
}
function width(t: Uint8Array | Int16Array) {
  // expect-error: property 'BYTES_PER_ELEMENT' of 'Uint8Array | Int16Array' is not supported yet; narrow it first
  return t.BYTES_PER_ELEMENT
}
function other(t: Uint8Array | Int16Array) {
  // expect-error: property 'size' does not exist on 'Uint8Array', which 'Uint8Array | Int16Array' may be; narrow it first
  return t.size
}
console.log(offset(new Uint8Array(1)), count(new Uint8Array(1)), width(new Uint8Array(1)),
  other(new Uint8Array(1)))
