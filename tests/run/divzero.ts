// expect-exit: 2
// expect-stderr: panic: integer divide by zero
let a: int = 10
let b: int = 0
console.log("before")
console.log(a / b)
console.log("after")
