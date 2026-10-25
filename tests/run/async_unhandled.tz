// same-as-node
// expect-exit: 1
// expect-stderr: Error: lost
// A rejected Promise that nothing handles ends the program, as in Node: once
// the nextTick callbacks and the microtasks of the turn are done, the error
// and its stack go to stderr, and the exit code is 1. Timers set before do
// not run.
async function save(name: string) {
  await undefined
  if (name == "") throw new Error("lost")
  console.log("saved", name)
}
void save("a")
void save("")
process.nextTick(() => console.log("nextTick runs first"))
void Promise.resolve().then(() => console.log("microtask runs first"))
setTimeout(() => console.log("never"), 0)
console.log("main")
