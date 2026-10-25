// Async syntax that is wrong: `for await` goes over the values of an
// iterable, with `of`.
const o = { a: 1 }
// expect-error: 'for await' goes over the values of an iterable, with 'of': 'for await (const x of xs)'
for await (const k in o) console.log(k)
// expect-error: 'for await' goes over the values of an iterable: 'for await (const x of xs)'
for await (let i = 0; i < 2; i++) console.log(i)
class Reader {
  // expect-error: a constructor cannot be 'async'
  async constructor() {}
}
