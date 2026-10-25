// same-as-node
// A function goes where a function type with a result its own converts to
// is expected, as TypeScript allows: an async one where `T | Promise<T>`
// is, an int where a double is; and one with more parameters, optional,
// which get undefined.
class Item {
  n: int
  constructor(n: int) {
    this.n = n
  }
}
async function run(make: (n: int) => Item | Promise<Item>) {
  const item = await make(3)
  console.log(item.n)
}
await run(n => new Item(n))
await run(async n => new Item(n * 10))
async function named(n: int) {
  await new Promise<void>(resolve => setTimeout(resolve, 1))
  return new Item(n + 1)
}
await run(named)
const asValue: (n: int) => Item | Promise<Item> = async n => new Item(-n)
await run(asValue)
const twice: (x: int) => double = (x: int): int => x * 2
console.log(twice(21) / 4)
const extra = (n: int, label?: string): Item => new Item(label == undefined ? n : -1)
await run(extra)
async function fails(n: int): Promise<Item> {
  throw new Error(`no item ${n}`)
}
try {
  await run(fails)
} catch (e) {
  console.log(e.message)
}
