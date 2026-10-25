// Async with the rest of the language: callbacks whose result a method uses
// cannot be async, a union holds one Promise type, what `await` gives must
// make one type, and facts that another task may undo do not last across
// `await`.
function later<T>(value: T, ms: number): Promise<T> {
  return new Promise(resolve => setTimeout(() => resolve(value), ms))
}
const text = "a-b-c"
// expect-error: 'replace' uses what the function returns as 'string', but an async function returns a Promise; JavaScript would insert "[object Promise]" for each match: work out the replacements first, then replace with a function that is not async
const replaced = text.replace(/-/g, async m => m + m)
async function upper(m: string) {
  return m.toUpperCase()
}
// expect-error: 'replaceAll' uses what the function returns as 'string', but an async function returns a Promise
const all = text.replaceAll(/-/g, upper)
const xs = [1, 2, 3]
// expect-error: 'filter' uses what the function returns as 'boolean', but an async function returns a Promise; a Promise is always truthy, so JavaScript would take every element as passing
const kept = xs.filter(async x => x > 1)
// expect-error: 'find' uses what the function returns as 'boolean', but an async function returns a Promise
const found = xs.find(async x => x > 1)
// expect-error: 'every' uses what the function returns as 'boolean'
const each = xs.every(async x => x > 0)
// expect-error: 'reduce' uses what the function returns as 'int', but an async function returns a Promise; drop 'async', or await what the function needs before the call
const sum = xs.reduce(async (acc, x) => acc + x, 0)
// Fine: the accumulator is a Promise.
const chained = xs.reduce(async (acc, x) => (await acc) + x, Promise.resolve(0))

// expect-error: 'Promise<int>' and 'Promise<string>' cannot be told apart when the program runs: both are promises; use one Promise type, e.g. 'Promise<int | string>'
let two: Promise<int> | Promise<string> = later(1, 1)
// expect-error: 'Promise.all' of values that may be promises ('int | Promise<int>') is not supported yet; await each first, or make each a Promise
const mixed = await Promise.all([process.argv.length > 9 ? 1 : later(2, 1), later(3, 1)])
function maybe(n: int): int | Promise<int> {
  return n > 0 ? n : later(n, 1)
}
const maybes = [maybe(1), maybe(2)]
// expect-error: 'Promise.race' of values that may be promises
const raced = await Promise.race(maybes)
const byKey = new Map<string, Promise<int>>()
// expect-error: 'Promise.all' needs an array of promises, found 'Generator<Promise<int>>'; make an array of them: 'Promise.all([...values])'
const fromMap = await Promise.all(byKey.values())
const nothing: Promise<void> | int = process.argv.length > 9 ? 1 : later(0, 1).then(() => {})
// expect-error: 'await' of 'Promise<void> | int' gives the value of the Promise or the value itself, which make no one type; check what it holds first
const waited = await nothing
const p = later(1, 1)
// expect-error: a Promise is always truthy, so the condition is always true
if (p) console.log("always")

// Across `await`: a module variable or a captured one may change, a local
// stays as checked.
let shared: string | undefined = "x"
async function across() {
  let local: string | undefined = shared
  let captured: string | undefined = "c"
  const reset = () => { captured = undefined }
  if (local != undefined && shared != undefined && captured != undefined) {
    await later(0, 1)
    console.log(local.length)
    // expect-error: the value may be undefined; check it before reading 'length'
    console.log(shared.length)
    // expect-error: the value may be undefined; check it before reading 'length'
    console.log(captured.length)
  }
  reset()
}
await across()
