// Decimals reach up to 10^10000000, as BigNumber.js by default; beyond that
// it gives Infinity, and tz throws a RangeError.
// expect-exit: 1
// expect-stderr: RangeError: Maximum decimal size exceeded
const ten: decimal = 10
console.log((ten ** 10000000).toPrecision(3), (ten ** -10000000).isInteger())
console.log(decimal(-4).abs().sqrt())
let n = 9999999
for (let i = 0; i < 3; i++) {
  console.log((ten ** n).decimalPlaces(), (ten ** n).toExponential())
  n++
}
