// same-as-node
// Output is shared: a piece that prints keeps node's order with the others
// that print (docs/async.md, sections 2 and 3), so the lines come out as in
// node on every run, and so does a computation between two prints of one
// piece. Pieces that print nothing run in parallel with those that do.
async function work(n: int) {
  console.log("start", n)
  await Promise.resolve()
  let sum = 0
  for (let i = 0; i < 1000; i++) sum += i
  console.log("done", n, sum)
  return n
}

console.log(await Promise.all([work(1), work(2), work(3)]))

async function quiet(n: int) {
  await Promise.resolve()
  return n * n
}

console.log(await Promise.all([quiet(5), work(4)]))
