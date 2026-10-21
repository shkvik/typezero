// same-as-node
// switch compares with ===, runs the first matching case and falls through
// into the next ones until a break.
function name(n: int): string {
  switch (n) {
    case 0:
      return "zero"
    case 1:
    case 2:
      return "small"
    case -1:
      return "minus one"
    default:
      return "other"
  }
}
console.log(name(0), name(1), name(2), name(-1), name(7))

function fallthrough(n: int): string {
  let text = ""
  switch (n) {
    case 1:
      text += "one "
    case 2:
      text += "two "
      break
    case 3:
      text += "three "
    default:
      text += "default "
    case 4:
      text += "four"
  }
  return text
}
for (let i = 0; i <= 5; i++) console.log(i, fallthrough(i))

// Strings, and a default that is not the last case.
const words = ["apple", "kiwi", "plum", "fig"]
for (const word of words) {
  switch (word) {
    default:
      console.log(word, "is some fruit")
      break
    case "apple":
    case "plum":
      console.log(word, "grows on trees")
      break
    case "kiwi":
      console.log(word, "grows on vines")
  }
}

// Numbers of different types compare as numbers; without a match and
// without default nothing runs.
const d: double = 2
switch (d) {
  case 1.5:
    console.log("1.5")
    break
  case 2:
    console.log("two as a double")
    break
}
switch (3) {
  case 1:
    console.log("never")
}

// break leaves the switch, continue the loop around it.
let odd = 0
for (let i = 0; i < 6; i++) {
  switch (i % 2) {
    case 0:
      continue
    case 1:
      odd++
      break
  }
  console.log("after switch", i)
}
console.log("odd", odd)

// A break inside a loop inside a case leaves only the loop.
switch (1) {
  case 1:
    for (let i = 0; ; i++) {
      if (i == 2) break
      console.log("loop in case", i)
    }
    console.log("still in case 1")
}

// Variables declared in a case, released when the switch ends.
function describe(items: string[]): string {
  switch (items.length) {
    case 0:
      return "nothing"
    case 1: {
      const only = items[0] ?? ""
      return "just " + only
    }
    default:
      const joined = items.join(" and ")
      return joined + " (" + items.length + ")"
  }
}
console.log(describe([]), "|", describe(["tea"]), "|", describe(["tea", "milk"]))

// The discriminant is evaluated once; tests run in order until one matches.
let calls = 0
function next(): int {
  calls++
  return calls
}
function test(label: string, value: int): int {
  console.log("test", label)
  return value
}
switch (next()) {
  case test("a", 0):
    console.log("matched a")
    break
  case test("b", 1):
    console.log("matched b")
    break
  case test("c", 1):
    console.log("matched c")
}
console.log("calls", calls)

// A value that may be undefined.
function maybe(n: int): int | undefined {
  if (n > 0) return n
}
for (let n = 0; n < 3; n++) {
  const m = maybe(n)
  switch (m) {
    case undefined:
      console.log("undefined")
      break
    case 1:
      console.log("one")
      break
    default:
      console.log("value", m + 10)
  }
}

// Nested switches.
for (const a of [1, 2]) {
  for (const b of ["x", "y"]) {
    switch (a) {
      case 1:
        switch (b) {
          case "x":
            console.log("1x")
            break
          default:
            console.log("1 and", b)
        }
        break
      default:
        console.log(a, b)
    }
  }
}

// Leaving a switch on a string any way releases it: return, break, the end.
function kind(s: string): int {
  switch (s + "!") {
    case "a!":
      return 1
    case "bb!": {
      const copy = s.repeat(20)
      if (copy.length > 10) return 2
      break
    }
    default:
      for (const ch of s) {
        if (ch == "z") return 3
      }
  }
  return 0
}
let total = 0
for (let i = 0; i < 100; i++) total += kind("a") + kind("bb") + kind("xyz") + kind("q")
console.log("total", total)
const names: (string | undefined)[] = ["x", undefined, "a longer string value"]
for (const n of names) {
  switch (n) {
    case undefined:
      console.log("none")
      continue
    case "x":
      console.log("x")
      break
    default:
      console.log(n.toUpperCase())
  }
}
