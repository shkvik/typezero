// same-as-node
// Conditions test strings, booleans and values that may be undefined, as in
// JavaScript; `&&` and `||` give one of their operands.
function find(names: string[], i: int): string | undefined {
  return names[i]
}
const names = ["ann", "", "bob"]

// A string is truthy unless empty; `T | undefined` when it holds such a value.
for (let i = 0; i < 4; i++) {
  const name = find(names, i)
  if (name) console.log(i, "has", name, name.length)
  else console.log(i, "empty or missing")
  console.log(!name, name ? "yes" : "no", Boolean(name))
}
let text = "abc"
while (text) text = text.slice(1)
console.log(`[${text}]`, !text)

// After `if (!x) return`, x holds a value.
function shout(word: string | undefined): string {
  if (!word) return "(nothing)"
  return word.toUpperCase()
}
console.log(shout("hey"), shout(""), shout(undefined))

// An array or a function that may be undefined: is it there?
function evens(n: int): int[] | undefined {
  if (n < 0) return undefined
  const result: int[] = []
  for (let i = 0; i < n; i += 2) result.push(i)
  return result
}
for (const n of [-1, 0, 5]) {
  const list = evens(n)
  if (list) console.log(n, "list of", list.length)
  else console.log(n, "no list")
  console.log(list && list.length, list || [9])
}
function run(task?: () => string): string {
  return task ? task() : "no task"
}
console.log(run(), run(() => "ran"))

// `boolean | undefined`: true, false or missing.
const flags: (boolean | undefined)[] = [true, false, undefined]
for (const flag of flags) console.log(flag, !flag, flag || "default".length > 3, flag && true)

// `||` and `&&` give one of the values.
for (let i = 0; i < 4; i++) {
  const nick = find(names, i)
  const title = nick || "anon"
  const both = nick && `${nick}!`
  console.log(title, both, nick || undefined)
}
console.log("" || "right", "left" || "right", "" && "right", "left" && "right")
// In a condition only the truth matters.
const first = find(names, 0)
if (first && first.length > 2) console.log("long first")
if (find(names, 1) || find(names, 2)) console.log("some name")

// `||=` and `&&=` follow the same rule.
let label: string | undefined
label ||= "set"
console.log(label)
label &&= label.toUpperCase()
console.log(label)
let note = ""
note ||= "filled"
note &&= `${note}.`
console.log(note)

// The left side is evaluated once, and the right side only when needed.
let calls = 0
function counted(value: string): string {
  calls++
  return value
}
console.log(counted("a") || counted("b"), counted("") || counted("c"), calls)

// The right side of `||` is in tail position.
function firstNonEmpty(words: string[], i: int): string | undefined {
  if (i >= words.length) return undefined
  return words[i] || firstNonEmpty(words, i + 1)
}
console.log(firstNonEmpty(["", "", "x"], 0), firstNonEmpty(["", ""], 0))
console.log(["a", "", "b"].filter(Boolean), Boolean(""), Boolean("0"))
