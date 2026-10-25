// same-as-node
// throw, try, catch and finally, as in JavaScript (docs/errors.md).
function parse(text: string): int {
  if (text.length == 0) throw new Error("empty")
  if (text == "x") throw new TypeError("not a number: " + text)
  return text.length
}

for (const text of ["abc", "", "x"]) {
  try {
    console.log("parsed", parse(text))
  } catch (e) {
    console.log("caught", e.name, e.message, String(e), `${e}`, e.toString())
  }
}

// catch without a variable, and the kinds.
try {
  parse("")
} catch {
  console.log("caught without a variable")
}
function kind(e: Error): string {
  if (e instanceof TypeError) return "TypeError"
  if (e instanceof RangeError) return "RangeError"
  if (e instanceof SyntaxError) return "SyntaxError"
  return e instanceof Error ? "Error" : "?"
}
const errors = [new Error("a"), new TypeError("b"), new RangeError("c"), new SyntaxError("d")]
for (const e of errors) console.log(kind(e), e.name, e.message)

// finally runs on every way out: normally, on a throw, on return and break.
function steps(mode: int): string {
  const log: string[] = []
  try {
    log.push("try")
    if (mode == 1) throw new Error("boom")
    if (mode == 2) return log.join(",") + " returned"
    log.push("end of try")
  } catch (e) {
    log.push("catch " + e.message)
  } finally {
    log.push("finally")
  }
  return log.join(",")
}
for (let mode = 0; mode < 3; mode++) console.log(steps(mode))

for (let i = 0; i < 5; i++) {
  try {
    if (i == 1) continue
    if (i == 3) break
    console.log("loop", i)
  } finally {
    console.log("finally", i)
  }
}

// An error goes on up after finally, through functions.
function inner(n: int): int {
  try {
    if (n > 2) throw new RangeError(`too big: ${n}`)
    return n * 2
  } finally {
    console.log("inner finally", n)
  }
}
function outer(n: int): int {
  return inner(n) + 1
}
try {
  console.log(outer(1))
  console.log(outer(5))
  console.log("not reached")
} catch (e) {
  console.log("outer caught", e.message)
}

// return in finally replaces the error, as in JavaScript.
function swallow(): string {
  try {
    throw new Error("lost")
  } finally {
    return "from finally"
  }
}
console.log(swallow())

// A throw in catch goes up, after the finally.
function rethrow(): void {
  try {
    throw new Error("first")
  } catch (e) {
    throw new Error("second, after " + e.message, { cause: e })
  } finally {
    console.log("rethrow finally")
  }
}
try {
  rethrow()
} catch (e) {
  console.log(e.message, e.cause?.message)
}

// The caught error can be thrown again, and is the same object.
let first: Error | undefined
try {
  try {
    throw new Error("again")
  } catch (e) {
    first = e
    throw e
  }
} catch (e) {
  console.log(e.message, e == first)
}

// Nested tries, and variables in scope released on the way.
function nested(): string {
  let text = "start"
  try {
    const parts = ["a", "b"]
    try {
      text = parts.join("+")
      throw new SyntaxError("inner")
    } finally {
      text += " (inner finally)"
    }
  } catch (e) {
    return `${text}: ${e}`
  }
  return text
}
console.log(nested())

// Methods and constructors throw too.
class Account {
  balance = 0
  owner: string
  constructor(owner: string) {
    if (owner.length == 0) throw new Error("no owner")
    this.owner = owner
  }
  withdraw(amount: int): int {
    if (amount > this.balance) throw new RangeError(`cannot withdraw ${amount}`)
    this.balance -= amount
    return this.balance
  }
}
try {
  const a = new Account("ann")
  a.balance = 10
  console.log(a.withdraw(4))
  console.log(a.withdraw(40))
} catch (e) {
  console.log(e.message)
}
try {
  new Account("")
} catch (e) {
  console.log(e.message)
}

// Through callbacks and closures.
const numbers = [1, 2, 3, 4]
try {
  const doubled = numbers.map(n => {
    if (n == 3) throw new Error(`bad ${n}`)
    return n * 2
  })
  console.log(doubled)
} catch (e) {
  console.log("map:", e.message)
}
const check = (n: int): int => {
  if (n < 0) throw new RangeError("negative")
  return n
}
const checks: { run: (n: int) => int } = { run: check }
try {
  console.log(checks.run(5), checks.run(-1))
} catch (e) {
  console.log("value call:", e.message)
}

// Errors with a message, with none, and fields that can be set.
const plain = new Error()
console.log(`[${plain.message}]`, plain.name, String(plain))
const named = new Error("custom")
named.name = "ParseError"
console.log(String(named))
