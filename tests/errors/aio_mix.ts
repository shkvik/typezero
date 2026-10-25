// What `for await` and AsyncIterable take: an async iterator, or an object
// whose `[Symbol.asyncIterator]()` is an async generator.
class Odd {
  [Symbol.asyncIterator]() {
    return 1
  }
}
async function collect(source: AsyncIterable<string>) {
  const out: string[] = []
  for await (const x of source)
    out.push(x)
  return out
}
// expect-error: '[Symbol.asyncIterator]()' of 'Odd' gives 'int', which is not an async iterator; make it an async generator
for await (const x of new Odd())
  console.log(x)
// expect-error: type 'string[]' is not assignable to type 'AsyncGenerator<string>'
console.log(await collect(["a"]))
