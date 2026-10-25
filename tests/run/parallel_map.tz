// same-as-node
// Tasks that update one Map keep node's order where they touch it, since its
// keys are known only when the program runs (docs/async.md, section 4). The
// piece that reads the cache, computes and writes it keeps that order as a
// whole: in node no other task comes between the `get` and the `set`.
const cache = new Map<string, int>()
let computed = 0

async function lookup(key: string) {
  await Promise.resolve()
  const hit = cache.get(key)
  if (hit != undefined) return hit
  computed++
  const value = key.length * 10
  cache.set(key, value)
  return value
}

console.log(await Promise.all([lookup("a"), lookup("bb"), lookup("a")]))
console.log(computed, cache.size)

// A Map that each task makes for itself is its own.
async function letters(text: string) {
  const counts = new Map<string, int>()
  await Promise.resolve()
  for (const ch of text) counts.set(ch, (counts.get(ch) ?? 0) + 1)
  return counts.size
}

console.log(await Promise.all([letters("hello"), letters("world")]))
