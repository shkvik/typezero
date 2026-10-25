// same-as-node
// Errors thrown while dictionaries are built and walked: by a callback, a
// generator they are made from, an element of a tuple or an object literal;
// nothing is left behind.
function attempt(what: string, f: () => string) {
  try {
    console.log(what, f())
  } catch (e) {
    console.log(what, `${e}`)
  }
}
function check(n: number): number {
  if (n > 2) throw new RangeError(`too big: ${n}`)
  return n
}
const m = new Map<string, number>([["a", 1], ["b", 2], ["c", 3]])
attempt("forEach", () => {
  let sum: number = 0
  m.forEach((v) => (sum += check(v)))
  return `${sum}`
})
attempt("groupBy", () => JSON.stringify(Object.groupBy([1, 2, 3], (n) => `${check(n)}`)))
attempt("map groupBy", () => `${Map.groupBy(["x", "yy", "zzz"], (s) => check(s.length)).size}`)
function* pairs(n: number): Generator<[string, number]> {
  for (let i = 0; i < n; i++) yield [`k${i}`, check(i)]
}
attempt("new Map", () => `${new Map(pairs(2)).size}`)
attempt("new Map throws", () => `${new Map(pairs(5)).size}`)
attempt("new Set", () => `${new Set([...m.values()].map(check)).size}`)
attempt("keys map", () => [...m.keys()].map((k) => `${k}${check(m.get(k) ?? 0)}`).join())
attempt("values helper", () =>
  m
    .values()
    .map((v) => check(v))
    .toArray()
    .join(),
)
attempt("for of", () => {
  const out: string[] = []
  for (const [k, v] of m) out.push(`${k}=${check(v)}`)
  return out.join()
})
attempt("record", () => {
  const r: Record<string, number> = {}
  for (const [k, v] of m) r[k] = check(v)
  return JSON.stringify(r)
})
attempt("fromEntries", () =>
  JSON.stringify(Object.fromEntries([...m].map(([k, v]): [string, number] => [k, check(v)]))),
)
attempt("spread", () => JSON.stringify({ ...{ a: "x", n: 1 }, n: check(5) }))
attempt("tuple", () => JSON.stringify([...m].map(([k, v]): [string, number] => [k, check(v)])))
