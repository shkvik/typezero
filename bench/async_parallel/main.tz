// CPU work in async tasks: 64 tasks at once, each counts the primes in a
// range of its own after an await, then Promise.all of the counts. The
// pieces after the await touch only their own data, so the parallel runtime
// runs them on all the cores (docs/async.md, section 2).
function isPrime(n: int): boolean {
  if (n < 2) return false
  for (let d = 2; d * d <= n; d++) if (n % d == 0) return false
  return true
}

async function count(from: int, to: int) {
  await Promise.resolve()
  let found = 0
  for (let n = from; n < to; n++) if (isPrime(n)) found++
  return found
}

const tasks: Promise<int>[] = []
for (let i = 0; i < 64; i++) tasks.push(count(i * 100000, (i + 1) * 100000))
const counts = await Promise.all(tasks)
let total = 0
for (const c of counts) total += c
console.log(total)
