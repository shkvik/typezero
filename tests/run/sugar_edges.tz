// same-as-node
// Corners of destructuring, `?.`, `!`, spread and the comma operator, run with
// the leak check: values in hidden variables, chains that stop early, and
// values dropped by the comma operator must all be released.
function maybe(n: int): string | undefined {
  return n > 0 ? `  text ${n}  ` : undefined
}
for (let i = -1; i <= 1; i++) {
  const s = maybe(i)
  console.log(s?.trim().toUpperCase(), s?.trim().length ?? -1, s?.split(" ")?.[2]?.at(0))
}

const lists: string[][] = [["a", "b"], []]
for (let i = 0; i < 3; i++) console.log(lists[i]?.at(-1)?.repeat(2), lists[i]?.length)

let names: string[] | undefined
console.log(names?.push("x"), names)
names = ["y"]
console.log(names?.push("x"), names)
console.log(names?.at(names?.length ?? 0), names?.indexOf(names?.[1] ?? ""))

// After `x!` the variable is known to hold a value.
let pick: string | undefined = names?.[0]
const upper = pick!.toUpperCase()
console.log(upper, pick.length)

// Destructured globals are visible in functions.
const [width, height] = [3, 4]
function area(): int {
  return width * height
}
console.log(area())

// Hidden variables hold strings and arrays until their scope ends.
for (let round = 0; round < 2; round++) {
  const [w1, w2 = "-", ...more] = `one two three ${round}`.split(" ")
  let [m, n] = [[w1 ?? ""], more]
  ;[m, n] = [n, m]
  console.log(w1, w2, more, m, n)
}

// The comma operator drops the values before the last one.
let text = "a"
const last = (text + "b", [text, text], text.repeat(3))
console.log(last)

// Spreads of strings and arrays into rest parameters.
function joinAll(sep: string, ...parts: string[]): string {
  return parts.join(sep)
}
console.log(joinAll("-", ..."abc", ...["x", "y"], "z"), joinAll("+"))
