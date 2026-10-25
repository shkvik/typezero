// safeTry used wrongly: it takes a generator written in the call, whose
// `yield*` takes Results and returns their errors from it.
function parse(s: string): Result<int, string> {
  return s == "" ? err("empty") : ok(s.length)
}
function name(s: string): Result<string, string> {
  return s == "" ? err("empty") : ok(s.trim())
}
function code(n: int): Result<int, int> {
  return n < 0 ? err(n) : ok(n)
}

// expect-error: safeTry takes a generator function written in the call: 'safeTry(function* () { ... })'
const a = safeTry(() => ok(1))
// expect-error: safeTry takes a generator function
const b = safeTry(function () {
  return ok(1)
})
// A generator that gives values delegates with `yield*` to an iterable; a
// Result's value is taken that way only in the generator of safeTry.
function* numbers() {
  yield 1
  // expect-error: 'yield*' of a Result takes its value only in the generator given to safeTry
  yield* parse("2")
}

// `yield*` returns from the generator: nothing computed before it in its
// statement may be waiting for it.
const c = safeTry(function* () {
  // expect-error: 'yield*' cannot be used here: when the Result is an Err it returns from the generator
  const list = [1, yield* parse("a")]
  const text = "x"
  // expect-error: 'yield*' cannot be used here
  const joined = text.trim() + (yield* name("b"))
  // expect-error: 'yield*' cannot be used here
  console.log(text.toUpperCase(), yield* parse("c"))
  const box = { n: 0 }
  // expect-error: 'yield*' cannot be used here
  box.n = yield* parse("d")
  return ok(list.length + joined.length + box.n)
})

const d = safeTry(function* () {
  // expect-error: 'yield*' in the generator of safeTry takes a Result, found 'int'
  const n = yield* 5
  return ok(n)
})

function declared(): Result<int, string> {
  return safeTry(function* () {
    // expect-error: 'yield*' returns the error 'int' when the Result is an Err, but the generator returns 'Result<int, string>'; convert the error with 'mapErr'
    const n = yield* code(1)
    return ok(n)
  })
}

function codes(n: int): Result<int, int[]> {
  return n > 0 ? ok(n) : err([n])
}
function names(s: string): Result<int, string[]> {
  return s == "" ? err([s]) : ok(s.length)
}
const e = safeTry(function* () {
  const n = yield* names("x")
  // expect-error: 'yield*' returns the error 'int[]' here, and the generator returns errors of 'string[]' elsewhere, which no union holds
  const m = yield* codes(n)
  return ok(m)
})

// expect-error: the generator of safeTry must return a Result, e.g. 'return ok(value)'
const f = safeTry(function* () {
  const n = yield* parse("x")
  console.log(n)
})

const g = safeTry(function* () {
  const n = yield* parse("x")
  // expect-error: the generator of safeTry gives no values: 'yield x' has no one to take x; 'yield* r' takes the value of the Result r
  yield n
  return ok(n)
})

// expect-error: the generator of safeTry takes no parameters
const h = safeTry(function* (x: int) {
  return ok(x)
})
