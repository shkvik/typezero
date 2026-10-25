// same-as-node
// When the code can catch the rejection of Promise.all, it goes on while the
// other tasks still run, so those may run at the same time as what comes
// after the await: they keep node's order with it (docs/async.md, section 3).
async function step(n: int) {
  await Promise.resolve()
  if (n == 1) throw new Error(`step ${n} failed`)
  await Promise.resolve()
  console.log("step", n, "done")
}

try {
  await Promise.all([step(1), step(2)])
} catch (e) {
  console.log("caught:", e.message)
}
console.log("after")

// Without a try the rejection would end the program, so the tasks end
// before the code after the await.
await Promise.all([step(3), step(4)])
console.log("end")
