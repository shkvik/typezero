// Result.fromThrowable turns a function that throws into one that returns a
// Result, and _unsafeUnwrap goes the other way (docs/errors.md, section 4).
function parseAge(text: string): int {
  const n = parseInt(text)
  if (isNaN(n)) throw new SyntaxError(`not a number: ${text}`)
  if (n < 0) throw new RangeError("negative")
  return n | 0
}

// Without a second function the error of the Result is the Error itself.
const safeAge = Result.fromThrowable(parseAge)
for (const text of ["42", "x", "-3"])
  console.log(safeAge(text).match(n => `age ${n}`, e => `${e.name}: ${e.message}`))

// With one, what it makes of the Error.
const withMessage = Result.fromThrowable(parseAge, e => e.message)
console.log(withMessage("7"), withMessage("-1"))
console.log(withMessage("x").map(n => n * 2).unwrapOr(-1))

// Any function value, an arrow too; one that returns nothing gives ok().
const always = Result.fromThrowable(() => {
  throw new Error("always")
})
console.log(always().isErr())
const quiet = fromThrowable((s: string) => console.log("quiet", s))
console.log(quiet("hi"))
const words = ["a", "bb"]
const lengthOf = Result.fromThrowable((i: int) => {
  const w = words[i]
  if (w == undefined) throw new RangeError(`no word ${i}`)
  return w.length
}, e => e.message)
console.log(lengthOf(1), lengthOf(5))

// A function that turns the error and throws itself: that goes on up.
const broken = Result.fromThrowable(parseAge, (e): string => {
  throw new Error("could not turn " + e.message)
})
try {
  console.log(broken("x"))
} catch (e) {
  console.log(e.message)
}

// _unsafeUnwrap and _unsafeUnwrapErr throw an Error for the other half.
const good: Result<int, string> = ok(5)
const bad: Result<int, string> = err("nope")
console.log(good._unsafeUnwrap(), bad._unsafeUnwrapErr())
for (const r of [good, bad]) {
  try {
    console.log("value", r._unsafeUnwrap())
  } catch (e) {
    console.log(`${e}`)
  }
  try {
    console.log("error", r._unsafeUnwrapErr())
  } catch (e) {
    console.log(`${e}`)
  }
}
