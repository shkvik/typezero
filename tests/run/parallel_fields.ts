// same-as-node
// Different fields of one object are different data, and so are the objects
// each task makes for itself: such tasks run in parallel (docs/async.md,
// section 4).
const stats = { read: 0, written: 0 }

async function reader() {
  await Promise.resolve()
  stats.read++
}

async function writer() {
  await Promise.resolve()
  stats.written += 2
}

async function own(n: int) {
  const box = { value: n }
  await Promise.resolve()
  box.value *= 2
  return box.value
}

await Promise.all([reader(), writer()])
console.log(stats)
console.log(await Promise.all([own(1), own(2)]))
