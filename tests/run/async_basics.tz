// same-as-node
// Async functions run up to their first `await` when called, then go on in
// a later turn of the microtask queue; `await` of a value that is no Promise
// takes a turn too (docs/async.md).
async function add(a: int, b: int) {
  return a + b
}

async function greet(name: string) {
  console.log("greet starts", name)
  await undefined
  console.log("greet goes on", name)
  return `hello, ${name}`
}

async function main() {
  const p = greet("Ann")
  console.log("greet returned a Promise")
  console.log(await p)
  const x = await add(1, 2)
  const y = await 5
  const z = (await add(x, y)) * 2
  console.log(x, y, z)
  // Values of every kind come through.
  console.log(await add(4, 1), await 0.5, await "text", await [1, 2, 3], await { a: 1 }, await true)
}

console.log("before main")
const done = main()
console.log("after main")
await done
console.log("done")

// The order of the parts of two tasks that wait for each other.
async function first() {
  console.log("first 1")
  await undefined
  console.log("first 2")
  await undefined
  console.log("first 3")
}
async function second() {
  console.log("second 1")
  await first()
  console.log("second 2")
}
void second()
console.log("top")
