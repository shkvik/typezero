// same-as-node
// x.toString(radix), with the digits and the fraction V8 writes, and the kinds of Error
// called without 'new'.
const xs: double[] = [0, 1, 255, 256, -255, 0.5, 0.1, 3.14159, 1e21, 9007199254740994, -0, 1 / 3, 123.456, 1e-7, 0.000001, 4294967295]
for (const x of xs) console.log(x.toString(2), x.toString(16), x.toString(36), x.toString(8), x.toString(10))
const n: int = 255
console.log(n.toString(16), (n + 0x100).toString(16).slice(1))
try { (1).toString(process.argv.length + 40) } catch (e) { console.log(e.name, e.message) }
console.log(NaN.toString(16), Infinity.toString(2), (-Infinity).toString(36))
const e = TypeError("plain")
console.log(e instanceof TypeError, e.message, Error("x").message, RangeError("r").name)
