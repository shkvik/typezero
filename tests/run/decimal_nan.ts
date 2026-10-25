// A double that is NaN or infinite has no decimal: decimal(x) of one throws
// a RangeError, as BigInt(NaN) does.
// expect-exit: 1
// expect-stderr: RangeError: The number NaN cannot be converted to a decimal because
const values: double[] = [0.5, 1e300, Number("x")]
for (const v of values) console.log(decimal(v))
