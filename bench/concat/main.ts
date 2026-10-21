let s = ""
for (let i = 0; i < 200000; i++) s += "ab"
let t = ""
for (let i = 0; i < 100000; i++) {
  t += "ab"
  if (t.endsWith("q")) console.log("never")
}
console.log(s.length, t.length)
