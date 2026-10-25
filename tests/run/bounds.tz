// same-as-node
// Indices the compiler proves in range: each of these reads has the element
// type, with no check at run time. Other reads give undefined, as in
// JavaScript.
function sum(a: int[]): int {
  let s = 0
  for (let i = 0; i < a.length; i++) s += a[i]
  return s
}
function rev(a: int[]): int {
  let s = 0
  for (let i = a.length - 1; i >= 0; i--) s += a[i]
  return s
}
function pairs(a: int[]): int {
  let s = 0
  for (let i = 0; i + 1 < a.length; i++) s += a[i] * a[i + 1]
  for (let i = 1; i < a.length; i++) s += a[i - 1]
  for (let i = 0; i < a.length - 1; i++) s += a[i + 1]
  return s
}
function strided(a: int[]): int {
  let s = 0
  for (let i = 0; i < a.length; i += 2) s += a[i]
  for (let i = a.length - 1; i >= 0; i -= 3) s += a[i]
  return s
}
function guarded(a: int[], i: int): int {
  if (i >= 0 && i < a.length) return a[i]
  if (i < 0 || i >= a.length) return -1
  return a[i]
}
function lastPlusFirst(a: int[]): int {
  if (a.length == 0) return 0
  return a[a.length - 1] + a[0]
}
function whileLoop(a: int[]): int {
  let i = 0
  let s = 0
  while (i < a.length) {
    s += a[i]
    i++
  }
  return s
}
function powers(n: int): int {
  if (n < 0) return 0
  const f = new Array<int>(n + 1)
  f[0] = 1
  for (let i = 1; i <= n; i++) f[i] = f[i - 1] * 2
  return f[n]
}
function squares(n: int): int[] {
  const out: int[] = []
  for (let i = 0; i < n; i++) out[out.length] = i * i
  for (let i = 0; i < out.length; i++) out[i] += 1
  return out
}
function drain(a: int[]): int {
  let s = 0
  while (a.length > 0) s += a.pop()
  return s
}
function constLength(): int {
  const days = ["mo", "tu", "we"]
  return days[2].length + days.length
}
function nested(m: int[][]): int {
  let s = 0
  for (let i = 0; i < m.length; i++) {
    const row = m[i]
    for (let j = 0; j < row.length; j++) s += row[j]
  }
  return s
}
function codes(s: string): int {
  let n = 0
  for (let i = 0; i < s.length; i++) n += s.charCodeAt(i)
  for (let i = s.length - 1; i >= 0; i--) if (s[i] == "a") n++
  return n
}
const a = [1, 2, 3, 4]
console.log(sum(a), rev(a), pairs(a), strided([1, 2, 3, 4, 5, 6, 7]), guarded(a, 2), guarded(a, 9), lastPlusFirst(a), whileLoop(a))
console.log(powers(10), squares(5), drain([1, 2, 3]), constLength(), nested([[1, 2], [3]]), codes("banana"))

// Not proven: the result may be undefined (NaN for charCodeAt).
function at(xs: string[], i: int): string {
  return xs[i] ?? "-"
}
const word = "hi"
let k = 5
console.log(at(["x", "y"], 1), at(["x", "y"], 2), at(["x"], -1), word[k], word.charCodeAt(k), word.at(-1), word.at(-3))
console.log(a.at(-1), a.at(-5), [7, 8][k] == undefined)
