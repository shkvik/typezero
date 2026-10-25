// same-as-node
// Building long strings stays linear: appends go into spare capacity, other
// concatenations become ropes, and both survive reads in between.
let s = ""
for (let i = 0; i < 100000; i++) s += "x"
console.log(s.length)

let t = ""
for (let i = 0; i < 100000; i++) {
  t += "ab"
  if (t.endsWith("q")) console.log("never")
}
console.log(t.length, t.slice(-4))

// Prepending builds a deep rope; reading and freeing it must not recurse.
let u = ""
for (let i = 0; i < 20000; i++) u = "y" + u
console.log(u.length, u[0], u[19999])

let v = ""
for (let i = 0; i < 1000; i++) v = `${v}${i},`
console.log(v.length, v.slice(0, 20))

// Two strings appended to the same base must not see each other.
let base = "0123456789abcdef"
base += "!"
let left = base + "L"
let right = base + "R"
console.log(left, right, base)
