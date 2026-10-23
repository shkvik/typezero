// same-as-node
// `>>>` and `| 0` give what JavaScript gives. An int's `x >>> n` is unsigned:
// shifted by 1 to 31 bits it stays an int, otherwise it is a long; where only
// the low 32 bits matter (under `|`, `&`, `^`) it stays an int.
const neg = -5
let zero = 0
console.log(-1 >>> 0, neg >>> 0, neg >>> zero, neg >>> 1, neg >>> 32, (neg >>> 0) | 0, 5 >>> 0)
let x = 7
for (let i = 0; i < 3; i++) console.log(x >>> i, -x >>> i)
const u = neg >>> 0
console.log(u + 1, u % 7, u > 2147483647, u & 0xff, u ^ 3, [10, 20, 30, 40][neg >>> 30])
let y = -8
y >>>= 1
console.log(y)

// FNV-1a, the way hash functions are written in JavaScript.
function fnv1a(s: string) {
  let h = 2166136261 | 0
  for (let i = 0; i < s.length; i++) {
    h ^= s.charCodeAt(i)
    h = Math.imul(h, 16777619)
  }
  return h >>> 0
}
console.log(fnv1a(""), fnv1a("hello"), fnv1a("typezero") % 1000)

// `| 0` is ToInt32 for a long too: it keeps the low 32 bits, wherever it is.
// So does any `&`, `|` or `^` between a long and an int.
const big = 2166136261
let wide = 4294967296 * 3 + 7
wide |= 0
let mask = 4294967295
mask ^= 1
const typed: int = 2166136261 | 0
console.log(big | 0, 2166136261 | 0, typed, wide, mask, 0xdeadbeef & 0xffff, 0xdeadbeef ^ 1, 0x80000000 | 0)
