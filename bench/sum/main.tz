const a: int[] = []
for (let i = 0; i < 10000000; i++) a.push(i & 1023)
let total: long = 0
for (let r = 0; r < 20; r++) {
  let s: long = 0
  for (let i = 0; i < a.length; i++) s += a[i]
  total += s
}
console.log(total)
