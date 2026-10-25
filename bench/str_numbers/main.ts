// Numbers to strings and back: a million integers and three million doubles
// in templates, String() and toFixed(2), then parseInt(), Number() and
// parseFloat() of a million strings each.
let size = 0
const ints: string[] = []
const decimals: string[] = []
for (let i = 0; i < 1000000; i++) {
  const n = `${i * 79}`
  const price = `${i * 0.37}`
  size += n.length + price.length + String(i / 3.0).length + (i * 0.01).toFixed(2).length
  ints.push(n)
  decimals.push(price)
}
let sum: long = 0
let big = 0
for (const s of ints) sum += parseInt(s) | 0
for (const s of decimals) if (Number(s) > 1000) big++
for (const s of decimals) if (parseFloat(s) < 1000) big++
console.log(size, sum, big)
