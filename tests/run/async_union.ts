// same-as-node
// Promises with unions: Promise.race and any over promises of different
// values give their union, allSettled a tuple of results of each value, as
// in TypeScript; `await` of a union (or of `T | undefined`) waits for the
// Promise it may hold, or a turn of the queue for the other values.
function later<T>(value: T, ms: number): Promise<T> {
  return new Promise(resolve => setTimeout(() => resolve(value), ms))
}
async function failInt(m: string): Promise<int> {
  throw new Error(m)
}
async function failString(m: string): Promise<string> {
  throw new Error(m)
}

// Promise.race, any and allSettled.
const raced = await Promise.race([later(1, 20), later("two", 5)])
console.log(raced, typeof raced)
console.log(await Promise.any([later(3, 1), later("four", 30)]))
console.log(await Promise.any([failString("a"), later(12, 2)]))
console.log(await Promise.race([later(["x", "y"], 3), later({ k: 1 }, 1)]))
console.log(await Promise.race([7, later("eight", 1)]))
const settled = await Promise.allSettled([later(5, 1), later("six", 2)])
console.log(settled)
const [five, six] = settled
if (five.status == "fulfilled")
  console.log((five.value ?? 0) + 1)
if (six.status == "fulfilled")
  console.log(six.value?.toUpperCase())
const three = await Promise.allSettled([later(true, 1), later("x", 1), failInt("no")])
console.log(three.length, three[2].status, three[2].reason?.message, three[0].value)
const pair = await Promise.all([later(9, 1), later("ten", 2)])
console.log(pair)
console.log(await new Promise(resolve => resolve(pair.length > 1 ? 11 : "eleven")))

// await of `T | undefined` and of unions with a Promise.
let pending: Promise<int> | undefined = undefined
console.log(pending == undefined, await pending)
if (process.argv.length < 100)
  pending = later(1, 1)
if (pending)
  console.log(await pending)
async function plusOne(x: int | Promise<int>) {
  return (await x) + 1
}
console.log(await plusOne(1), await plusOne(later(10, 1)))
const either: string | Promise<string> = process.argv.length > 100 ? "now" : later("later", 1)
console.log((await either).toUpperCase())
class Foo {
  name: string
  constructor(name: string) {
    this.name = name
  }
}
function pick(n: int): Foo | Promise<Foo> {
  return n > 0 ? new Foo("direct") : later(new Foo("waited"), 1)
}
console.log((await pick(1)).name, (await pick(0)).name)
const some: int | Promise<int> | undefined = process.argv.length > 100 ? undefined : later(5, 1)
const none: int | Promise<int> | undefined = process.argv.length > 100 ? 1 : undefined
console.log(await some, await none)
class Job {
  run: Promise<string> | undefined = undefined
}
const job = new Job()
job.run = later("ran", 1)
console.log(job.run != undefined, await job.run)

// Each `await` takes the turns it takes in JavaScript.
const order: string[] = []
async function waitOn(label: string, x: int | Promise<int> | undefined) {
  order.push(`${label}1`)
  await x
  order.push(`${label}2`)
}
async function plain(label: string) {
  order.push(`${label}1`)
  await 0
  order.push(`${label}2`)
  await 0
  order.push(`${label}3`)
}
await Promise.all([waitOn("p", Promise.resolve(1)), plain("q"), waitOn("v", 2), waitOn("u", undefined),
                   waitOn("t", later(0, 1))])
console.log(order.join(" "))

// Promises in dictionaries, and unions with a Promise, print as in node.
const byName = new Map<string, Promise<int>>([["a", Promise.resolve(1)], ["b", later(2, 1)]])
console.log(await Promise.all([...byName.values()]), await Promise.race([...byName.values()]))
console.log(byName)
const rec: Record<string, Promise<string>> = { x: Promise.resolve("s") }
console.log(rec)
const u: int | Promise<int> = process.argv.length > 100 ? 1 : Promise.resolve(2)
console.log(u, [u])
const o: Promise<int> | undefined = process.argv.length > 100 ? undefined : Promise.resolve(3)
console.log(o, { o })
