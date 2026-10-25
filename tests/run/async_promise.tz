// same-as-node
// The functions of Promise and how console.log shows Promises.
const a = Promise.resolve(1)
const b = new Promise<string>(resolve => setTimeout(() => resolve("later"), 5))
console.log(a, b)
console.log(await Promise.all([a, b, Promise.resolve(3)]))
console.log(await Promise.all([1, 2, 3].map(async n => n * 10)))
console.log(await Promise.all([Promise.resolve(1), "two", true]))
const settled = await Promise.allSettled([Promise.resolve(1), Promise.reject(new Error("x"))])
for (const s of settled) console.log(s.status, s.value, s.reason?.message)
console.log(await Promise.allSettled([Promise.resolve("a"), Promise.resolve("b")]))
const slow = new Promise<int>(resolve => setTimeout(() => resolve(1), 60))
const fast = new Promise<int>(resolve => setTimeout(() => resolve(2), 5))
console.log("race", await Promise.race([slow, fast]))
console.log("any", await Promise.any([Promise.reject(new Error("a")), Promise.resolve(7)]))
try {
  await Promise.any([Promise.reject(new Error("a")), Promise.reject(new Error("b"))])
} catch (e) {
  console.log(e.name, e.message)
}
try {
  await Promise.all([Promise.resolve(1), Promise.reject(new Error("one fails"))])
} catch (e) {
  console.log("all:", e.message)
}
const none: Promise<int>[] = []
console.log(await Promise.all(none), await Promise.allSettled(none))
const { promise, resolve } = Promise.withResolvers<int>()
setTimeout(() => resolve(42), 1)
console.log(await promise)
console.log(await Promise.resolve(5).then(x => x + 1).then(x => `${x}!`))
const p = Promise.resolve([1, 2])
console.log(p, await p, { p })
console.log(new Promise<int>(() => {}))
console.log(Promise.resolve())
const tasks: Promise<void>[] = []
let done = 0
for (let i = 0; i < 1000; i++)
  tasks.push((async () => {
    await undefined
    done++
  })())
await Promise.all(tasks)
console.log("done", done)
