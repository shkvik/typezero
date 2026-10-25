// safeTry: `yield* r` takes the value of an Ok, or returns the Err from the
// generator, which runs once, as an ordinary function.
function parse(s: string): Result<int, string> {
  if (s.length == 0) return err("empty")
  const n = parseInt(s)
  return isNaN(n) ? err(`not a number: ${s}`) : ok(n | 0)
}

function sum(a: string, b: string): Result<int, string> {
  return safeTry(function* () {
    const x = yield* parse(a) // an Err returns from sum here
    const y = yield* parse(b)
    return ok(x + y)
  })
}
console.log(sum("1", "2"), sum("", "2"), sum("1", "x"))

// Without a declared type the generator's returns and its errors make one.
// The strings it holds are released whichever way it returns.
function product(a: string, b: string) {
  return safeTry(function* () {
    const label = `${a} * ${b}`
    const x = yield* parse(a).mapErr(e => `${label}: ${e}`)
    const y = yield* parse(b).mapErr(e => `${label}: ${e}`)
    return ok(`${label} = ${x * y}`)
  })
}
console.log(product("6", "7"), product("6", ""))

// `yield*` in an expression, after numbers and literals only; and as a
// statement.
const both = safeTry(function* () {
  yield* parse("1")
  console.log("first", yield* parse("9"), `then ${yield* parse("10")}`)
  return ok((yield* parse("2")) + (yield* parse("3")) * 10)
})
console.log(both)

// In a loop: the first Err ends it.
const steps = safeTry(function* () {
  let total = 0
  for (const s of ["5", "6", "y", "7"]) {
    total += yield* parse(s)
    console.log("total", total)
  }
  return ok(total)
})
console.log(steps)

// `yield* r.safeUnwrap()`, as neverthrow wrote it before, is the same.
const old = safeTry(function* () {
  return ok((yield* parse("8").safeUnwrap()) + 1)
})
console.log(old)

// Result.combine: the values, or the first Err; combineWithAllErrors: the
// values, or every error.
console.log(Result.combine([parse("1"), parse("2"), parse("3")]))
console.log(Result.combine([parse("1"), parse(""), parse("x")]))
console.log(Result.combineWithAllErrors([parse("1"), parse(""), parse("x")]))
console.log(Result.combineWithAllErrors(["4", "5"].map(parse)))
const none: Result<string, int>[] = []
console.log(Result.combine(none), Result.combineWithAllErrors(none))
const words: Result<string, int>[] = [ok("a"), ok("b")]
console.log(Result.combine(words).map(w => w.join("")))

// Results with values of different types give a tuple, as in neverthrow.
function named(s: string): Result<string, string> {
  return s.trim() == "" ? err("no name") : ok(s.trim())
}
console.log(Result.combine([named(" Ann "), parse("30")]), Result.combine([named(""), parse("")]))
console.log(Result.combineWithAllErrors([named(""), parse("1"), parse("")]))
const pair = Result.combine([named("Bo"), parse("41")])
if (pair.isOk()) {
  const [who, years] = pair.value
  console.log(who, years + 1)
}

// A recursive function's type is found through its generator too.
function depth(n: int) {
  if (n == 0) return ok("bottom")
  if (n < 0) return err(n)
  return safeTry(function* () {
    const below = yield* depth(n - 1)
    return ok(`${n}/${below}`)
  })
}
console.log(depth(3), depth(-2))
