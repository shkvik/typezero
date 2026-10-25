// same-as-node
// The ways out of try, catch and finally, and the values an error passes on
// its way up, which are released as it goes (the tests check for leaks).
function fail(what: string): string {
  if (what.length > 0) throw new Error(what)
  return "fine"
}

// Labeled jumps through finally, also through several of them.
outer: for (let i = 0; i < 3; i++) {
  for (let j = 0; j < 3; j++) {
    try {
      try {
        if (j == 1) continue outer
        if (i == 2) break outer
        console.log("at", i, j)
      } finally {
        console.log("inner finally", i, j)
      }
    } finally {
      console.log("outer finally", i, j)
    }
  }
}

// A switch inside a try, and a break out of it.
function pick(n: int): string {
  let result = ""
  try {
    switch (n) {
      case 1:
        result = "one"
        break
      default:
        throw new RangeError(`no ${n}`)
    }
  } catch (e) {
    result = e.message
  } finally {
    result += "!"
  }
  return result
}
console.log(pick(1), pick(2))

// The value a return gives waits while finally runs; a finally that returns
// or throws replaces it.
function waits(): string {
  const parts = ["a", "b"]
  try {
    return parts.join("") + "-returned"
  } finally {
    parts.push("c")
    console.log("finally sees", parts.length)
  }
}
console.log(waits())
function replaced(): string {
  try {
    return "first".toUpperCase()
  } finally {
    return "second"
  }
}
console.log(replaced())
function thrownOver(): string {
  try {
    return "lost".repeat(2)
  } finally {
    throw new Error("from finally")
  }
}
try {
  thrownOver()
} catch (e) {
  console.log(e.message)
}
function errorReplaced(): void {
  try {
    throw new Error("old")
  } finally {
    throw new TypeError("new")
  }
}
try {
  errorReplaced()
} catch (e) {
  console.log(`${e}`)
}

// A return from a catch, with a finally.
function fromCatch(): string {
  try {
    fail("x")
  } catch (e) {
    return "caught " + e.message
  } finally {
    console.log("finally after catch")
  }
  return "not reached"
}
console.log(fromCatch())

// Values that a throw leaves halfway are released: parts of a string, an
// array, an object, arguments.
const name = "world".toUpperCase()
const attempts: (() => void)[] = [
  () => console.log("hello " + name + fail("in a concatenation")),
  () => console.log(`${name}: ${fail("in a template")}`),
  () => console.log([name, fail("in an array")]),
  () => console.log({ who: name, what: fail("in an object") }),
  () => console.log(name.padStart(10), fail("in the arguments")),
  () => console.log(name.toLowerCase().concat(fail("after a receiver"))),
  () => console.log([name, name].map(s => s + fail("in map"))),
  () => console.log([name].filter(s => fail("in filter") == s)),
]
for (const attempt of attempts) {
  try {
    attempt()
  } catch (e) {
    console.log("caught", e.message)
  }
}

// The caught error in a closure, and a variable of the catch in a loop.
const messages: (() => string)[] = []
for (const word of ["one", "two"]) {
  try {
    fail(word)
  } catch (e) {
    messages.push(() => "closure saw " + e.message)
  }
}
for (const m of messages) console.log(m())

// A loop that retries until it works.
let tries = 0
function flaky(): string {
  tries++
  if (tries < 3) throw new Error(`try ${tries}`)
  return `worked after ${tries}`
}
while (true) {
  try {
    console.log(flaky())
    break
  } catch (e) {
    console.log("retry:", e.message)
  }
}

// Generators: an error in the body comes out where the values are taken,
// whether the generator runs in the loop or as an object.
function* numbers(limit: int) {
  for (let i = 0; i < 5; i++) {
    if (i == limit) throw new RangeError(`stop at ${i}`)
    yield i
  }
}
try {
  for (const n of numbers(2)) console.log("got", n)
} catch (e) {
  console.log(e.message)
}
const it = numbers(1)
try {
  console.log(it.next().value)
  console.log(it.next().value)
} catch (e) {
  console.log("next:", e.message)
}
console.log(it.next().done)
function* guarded() {
  try {
    yield 1
    yield 2
    throw new Error("inside")
  } catch (e) {
    console.log("generator caught", e.message)
    yield 3
  } finally {
    console.log("generator finally")
  }
}
console.log([...guarded()])
try {
  for (const n of numbers(9)) {
    if (n == 1) throw new Error(`body at ${n}`)
  }
} catch (e) {
  console.log(e.message)
}
