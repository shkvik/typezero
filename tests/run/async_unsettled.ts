// same-as-node
// expect-exit: 13
// Top-level code that waits for a Promise nothing will settle: when nothing
// else is left to do, the program ends with code 13, as Node does.
console.log("start")
setTimeout(() => console.log("the timer still runs"), 1)
await new Promise<void>(() => {})
console.log("never")
