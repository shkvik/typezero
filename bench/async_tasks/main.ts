// 100 thousand tasks at once, each awaiting ten times, then Promise.all of
// their results.
async function worker(id: int, rounds: int) {
  let acc = 0
  for (let r = 0; r < rounds; r++) {
    await undefined
    acc += (id * r) & 15
  }
  return acc
}

const tasks: Promise<int>[] = []
for (let i = 0; i < 100000; i++) tasks.push(worker(i, 10))
const results = await Promise.all(tasks)
let total = 0
for (const r of results) total += r
console.log(total)
