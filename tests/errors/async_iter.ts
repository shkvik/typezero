// Async generators and `for await` (docs/iterators.md, step 4): `for await`
// goes where `await` goes, over what can be iterated; an async generator
// gives an async iterator, and awaits what it yields.
const names = ["a"]
function sync() {
  // expect-error: 'for await' can only be used in an async function or in top-level code
  for await (const n of names) console.log(n)
}
// expect-error: 'for await' needs an async iterator, an array or an iterator, found 'int'
for await (const n of 5) console.log(n)
// expect-error: an async generator returns an async iterator: declare its type as 'AsyncGenerator<int>'
async function* wrong(): Generator<int> {
  yield 1
}
async function* typed(): AsyncGenerator<string> {
  // expect-error: the Promise gives 'int', but 'typed' yields 'string'
  yield Promise.resolve(1)
}
async function* numbers() {
  yield 1
}
// expect-error: 'next()' of an async iterator is not supported yet; iterate it with 'for await'
await numbers().next()
// expect-error: 'AsyncGenerator<int>' gives its values later: iterate it with 'for await (const x of ...)'
for (const n of numbers()) console.log(n)
sync()
