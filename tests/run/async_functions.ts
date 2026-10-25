// same-as-node --experimental-transform-types
// Async functions of every kind: methods that use `this`, closures, generic
// functions, arrows given to map, `return` of a Promise, `finally`.
class Store {
  items: string[] = []
  constructor(public name: string) {}
  async add(item: string) {
    await undefined
    this.items.push(item)
    return this.items.length
  }
  async addAll(list: string[]) {
    for (const item of list) await this.add(item)
    return `${this.name}: ${this.items.join(", ")}`
  }
}
const store = new Store("shop")
console.log(await store.addAll(["apple", "pear"]))

function counter() {
  let count = 0
  return async (step: int) => {
    await undefined
    count += step
    return count
  }
}
const next = counter()
console.log(await next(2), await next(3))

async function identity<T>(x: T): Promise<T> {
  await undefined
  return x
}
console.log(await identity(5), await identity("five"), await identity([1, 2]))

async function sumTo(n: int): Promise<int> {
  let total = 0
  for (let i = 1; i <= n; i++) total += await identity(i)
  return total
}
console.log(await sumTo(100))

const lengths = await Promise.all(["a", "bb", "ccc"].map(async s => {
  await undefined
  return s.length
}))
console.log(lengths)

async function retPromise(): Promise<string> {
  return identity("via promise")
}
console.log(await retPromise())

async function withFinally(): Promise<int> {
  try {
    return await identity(1)
  } finally {
    console.log("finally runs")
  }
}
console.log(await withFinally())

async function finallyPromise(): Promise<int> {
  try {
    return identity(2)
  } finally {
    console.log("finally before follow")
  }
}
console.log(await finallyPromise())

const obj = {
  n: 10,
  async double() {
    await undefined
    return this.n * 2
  },
}
console.log(await obj.double())
