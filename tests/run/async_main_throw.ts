// same-as-node
// expect-exit: 1
// expect-stderr: Error: top-level
// An error of top-level code ends the program at once, as without promises:
// the microtasks and nextTick callbacks it queued do not run. (An ES module,
// a file with `import`, `export` or top-level `await`, reports it a few
// turns of the microtask queue later: see async_await_throw.ts.)
let n = 0
function chain() {
  console.log("turn", n++)
  if (n < 10) queueMicrotask(chain)
}
queueMicrotask(chain)
process.nextTick(() => console.log("never: nextTick"))
console.log("main")
throw new Error("top-level")
