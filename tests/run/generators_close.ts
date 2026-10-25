// same-as-node
// A generator stopped before its end is closed, as return() in JavaScript:
// the `finally` blocks around the `yield` it waits at run, inner first.
const log: string[] = []
function* count(n: int, name: string): Generator<int> {
  try {
    try {
      for (let i = 1; i <= n; i++) yield i
    } finally {
      log.push(`${name} inner`)
    }
  } finally {
    log.push(`${name} outer`)
  }
}
function flush(what: string) {
  console.log(`${what}: [${log.join(", ")}]`)
  log.length = 0
}

let sum = 0
for (const x of count(5, "break")) {
  if (x == 3) break
  sum += x
}
flush(`break ${sum}`)

function firstOver(limit: int): int {
  for (const x of count(10, "return")) if (x > limit) return x
  return -1
}
flush(`return ${firstOver(4)}`)

try {
  for (const x of count(5, "throw")) if (x == 2) throw new RangeError(`at ${x}`)
} catch (e) {
  flush(`throw ${e.message}`)
}

// Closed even where a variable keeps it: the next loop gets nothing.
const kept = count(5, "kept")
for (const x of kept) if (x == 2) break
flush("kept")
console.log([...kept].length)
flush("kept again")

// To the end: the `finally` blocks run as the generator ends.
console.log([...count(3, "spread")].join())
flush("spread")

// Helpers that stop early close what they read.
console.log(count(9, "take").take(2).toArray().join())
flush("take")
console.log(count(9, "find").find((x) => x * x > 10))
flush("find")
console.log(count(9, "some").some((x) => x == 4))
flush("some")

// yield* passes the closing on to the generator it delegates to.
function* both(): Generator<int> {
  try {
    yield* count(2, "first")
    yield* count(2, "second")
  } finally {
    log.push("both")
  }
}
for (const x of both()) if (x == 2) break
flush("delegate")

// A label, and a `continue` that stays inside the loop.
outer: for (const a of count(3, "a")) {
  for (const b of count(3, "b")) {
    if (b == 2) continue
    if (a == 2) continue outer
    if (a == 3) break outer
  }
}
flush("labels")
