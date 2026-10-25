// same-as-node
// expect-exit: 1
// expect-stderr: Error: after await
// With top-level `await`, an error of top-level code is reported five turns
// of the microtask queue later.
let n = 0
function chain() {
  console.log("turn", n++)
  if (n < 10) queueMicrotask(chain)
}
console.log("main")
await undefined
queueMicrotask(chain)
await undefined
console.log("after")
throw new Error("after await")
