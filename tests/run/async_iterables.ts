// same-as-node
// Classes that `for await` iterates: `async *[Symbol.asyncIterator]()`, and
// `[Symbol.iterator]()` too; AsyncIterable<T> parameters given such objects
// and async generators; async generator methods of a generic class, also
// for T = unknown, and a generic async generator of them.
function later<T>(value: T, ms: number): Promise<T> {
  return new Promise(resolve => setTimeout(() => resolve(value), ms))
}

class Feed {
  items: string[]
  constructor(items: string[]) {
    this.items = items
  }
  async *[Symbol.asyncIterator]() {
    for (const item of this.items)
      yield await later(item.toUpperCase(), 1)
  }
}

class Countdown {
  from: int
  constructor(from: int) {
    this.from = from
  }
  *[Symbol.iterator]() {
    for (let i = this.from; i > 0; i--)
      yield i
  }
}

async function collect(source: AsyncIterable<string>): Promise<string[]> {
  const out: string[] = []
  for await (const x of source)
    out.push(x)
  return out
}

for await (const item of new Feed(["a", "b"]))
  console.log("feed", item)
for await (const n of new Countdown(3))
  console.log("count", n)
console.log(await collect(new Feed(["x", "y", "z"])))
async function* letters() {
  yield "p"
  yield "q"
}
console.log(await collect(letters()))

class Box<T> {
  value: T
  constructor(value: T) {
    this.value = value
  }
  async *repeat(times: int) {
    for (let i = 0; i < times; i++)
      yield this.value
  }
}
for await (const v of new Box(7).repeat(2))
  console.log("box", v)
async function* mapAsync<T, U>(source: AsyncIterable<T>, f: (x: T) => U) {
  for await (const x of source)
    yield f(x)
}
for await (const v of mapAsync(new Box("w").repeat(2), s => s.length))
  console.log("mapped", v)
const anything: unknown[] = []
for await (const v of new Box<unknown>("u").repeat(1))
  anything.push(v)
console.log(anything)
