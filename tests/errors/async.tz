// What async does not allow: a Promise nothing waits for, `await` outside
// an async function, an async function whose Promise a caller drops.
async function save(name: string) {
  await undefined
  console.log("saved", name)
}
const names = ["a", "b"]

// expect-error: the Promise is not awaited, so nothing waits for it and an error in it would end the program later; write 'await save("Ann")', or 'void save("Ann")' to let it run on its own
save("Ann")
// expect-error: the Promise of this async function is dropped here, so nothing waits for it to finish
names.forEach(async n => { await save(n) })
// expect-error: 'save' returns a Promise that 'forEach' drops
names.forEach(save)
// expect-error: the function returns a Promise that its caller drops
names.forEach(n => save(n))
// expect-error: the Promises are not awaited
names.map(n => save(n))
// expect-error: the Promise is not awaited
Promise.resolve(1).then(x => x + 1)
// Handled: catch(), then() with a second function, finally() after them.
save("x").catch(e => console.log(e.message))
save("y").then(() => {}, e => console.log(e.message))
save("z").catch(e => console.log(e.message)).finally(() => console.log("done"))
void save("dropped on purpose")

function plain() {
  // expect-error: 'await' can only be used in an async function or in top-level code; make the function async: 'async function plain'
  await save("x")
}
// expect-error: make the function async: 'async (...) => ...'
const arrow = () => await save("y")
class Box {
  run() {
    // expect-error: make the function async: 'async run() { ... }'
    await save("z")
  }
}

// expect-error: an async function returns a Promise: declare its type as 'Promise<int>'
async function wrong(): int {
  return 1
}
// expect-error: 'Promise' needs the type of its value
let untyped: Promise = Promise.resolve(1)
const p = Promise.resolve(1)
async function mismatch(): Promise<string> {
  // expect-error: is not assignable
  return 1
}
// expect-error: cannot tell the type of the Promise's value from what is given to 'resolve'
const unknownValue = new Promise(resolve => resolve(undefined))
// expect-error: type 'string' is not assignable to type 'Error'
const rejected = new Promise<int>((resolve, reject) => reject("no"))
// expect-error: arguments for the callback are not supported
setTimeout(save, 10, "x")
// expect-error: a Promise has 'then', 'catch' and 'finally'
p.done()
