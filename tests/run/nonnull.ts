// expect-exit: 2
// expect-stderr: panic: TypeError: the value asserted with '!' is undefined
// `x!` checks that x is there, and stops the program otherwise.
const a = [1, 2, 3]
let i = 1
i += 2
console.log(a[i - 1]!)
console.log(a[i]!)
console.log("not reached")
