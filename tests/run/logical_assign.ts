// same-as-node
// `x ||= v` and `x &&= v`: v is evaluated, and assigned, only when needed.
const words = ["apple", "kiwi", "banana"]
let long = false
for (const w of words) long ||= w.length > 5
let short = true
for (const w of words) short &&= w.length < 6
console.log(long, short)

let calls = 0
function check(v: boolean): boolean {
  calls++
  return v
}
let flag = true
flag ||= check(false) // already true: check is not called
console.log(flag, calls)
flag &&= check(false)
console.log(flag, calls)
flag &&= check(true) // already false
console.log(flag, calls)

// Elements at a simple index work too.
const seen = [false, true, false]
for (let i = 0; i < seen.length; i++) seen[i] ||= i == 2
console.log(seen)

// The value of the expression is the variable's new value.
let b = false
console.log((b ||= true), b, (b &&= false), b)
