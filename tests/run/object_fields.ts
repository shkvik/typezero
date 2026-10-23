// Optional fields. A field that is not there and one that holds undefined
// are the same: console.log shows neither (node shows `limit: undefined` for
// the second), and `x: T | undefined` declares the same field as `x?: T`.
interface Query {
  text: string
  limit?: int
  tags: string[] | undefined
}
const a: Query = { text: "cats", tags: undefined }
const b: Query = { text: "dogs", limit: undefined, tags: ["pets"] }
const c: Query = { text: "birds" }
console.log(a, b, c)
b.limit = 3
console.log(b)
b.limit = undefined
console.log(b, b.limit ?? 20)

// A field checked to hold a value is narrowed, until something may change
// it: an assignment to a field of that name, or to the variable.
function describe(q: Query): string {
  if (q.limit == undefined) return q.text
  return `${q.text} (${q.limit + 1})`
}
console.log(describe(a), describe({ text: "fish", limit: 9, tags: [] }))
function tagCount(q: Query): int {
  if (!q.tags) return 0
  q.limit = q.tags.length // a different field: q.tags stays narrowed
  return q.tags.length + q.limit
}
console.log(tagCount(b), b)
interface Node {
  value: int
  next?: Node
}
function last(node: Node): int {
  let current = node
  while (current.next) current = current.next
  return current.value
}
function secondOr(node: Node, fallback: int): int {
  return node.next ? node.next.value : fallback
}
const chain: Node = { value: 1, next: { value: 2, next: { value: 3 } } }
console.log(last(chain), secondOr(chain, 0), secondOr({ value: 5 }, -1))
if (chain.next != undefined && chain.next.next != undefined) console.log(chain.next.next.value)
if (chain.next) {
  const n = chain.next.value
  console.log(n, chain.next!.value)
}

// `??=`, `||=` and `&&=` on fields.
const settings: { name?: string; retries?: int; verbose?: boolean } = {}
settings.retries ??= 3
settings.retries ??= 5
settings.name ||= "default"
settings.verbose &&= false
console.log(settings, settings.retries)
settings.verbose = true
settings.verbose &&= false
settings.name ||= "other"
console.log(settings)
