// A million awaits of an async function that finishes at once, and a million
// awaits of a plain value: each takes a turn of the microtask queue.
async function next(i: int) {
  return i & 7
}

async function main() {
  let sum = 0
  for (let i = 0; i < 1000000; i++) sum += await next(i)
  for (let i = 0; i < 1000000; i++) sum += await (i & 3)
  return sum
}

console.log(await main())
