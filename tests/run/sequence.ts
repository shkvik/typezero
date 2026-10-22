// same-as-node
// The comma operator: in loop headers, and wherever an expression goes.
const s = "abcdef"
let reversed = ""
for (let i = 0, j = s.length - 1; j >= 0; i++, j--) reversed += s[j] ?? ""
console.log(reversed)

// Both indices are proven in range: no `?? 0` is needed.
const a = [1, 2, 3, 4, 5]
for (let i = 0, j = a.length - 1; i < j; i++, j--) {
  const t = a[i]
  a[i] = a[j]
  a[j] = t
}
console.log(a)

let steps = 0
for (let i = 0, n = 10; i < n; i += 3, steps++) {}
console.log(steps)

// The value is the last one; all are evaluated, in order.
let x = 0
const y = (x++, x++, x * 10)
console.log(x, y)

let k = 0
while ((k++, k < 3)) console.log("k =", k)

let log = ""
log += "a", log += "b", log += "c"
console.log(log)
console.log(`${(log += "!", log.length)}`)
