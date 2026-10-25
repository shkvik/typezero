// same-as-node
// An async generator runs in the task of the loop that iterates it: its
// pieces after an await are pieces of that task, and what follows each
// `yield` runs inside the loop's own piece (docs/async.md, section 2).
const log: string[] = []

async function* ticks(name: string, n: int) {
  for (let i = 0; i < n; i++) {
    await Promise.resolve()
    log.push(`${name}${i}`)
    yield i
  }
}

async function consume(name: string) {
  let sum = 0
  for await (const t of ticks(name, 3)) sum += t
  return sum
}

console.log(await Promise.all([consume("a"), consume("b")]))
console.log(log.join(" "))
