// same-as-node
// Closures capturing in loops, blocks and at the top level.
const fromForOf: (() => string)[] = []
for (const word of ["a", "b", "c"]) fromForOf.push(() => word.toUpperCase())
console.log(fromForOf.map(f => f()).join(""))

const fromWhile: (() => int)[] = []
let n = 0
while (n < 3) {
  const copy = n * 100
  fromWhile.push(() => copy + n)
  n++
}
console.log(fromWhile.map(f => f()))

// A parameter that is changed and captured lives in a box.
function stepper(start: int) {
  const step = () => start++
  step()
  step()
  return () => start
}
console.log(stepper(5)())

// Narrowing inside a closure.
function describe(x?: int) {
  const show = () => (x != undefined ? `value ${x}` : "nothing")
  return show()
}
console.log(describe(3), describe())

// Three levels.
function level1(a: int) {
  return (b: int) => {
    const c = a + b
    return (d: int) => a * 1000 + b * 100 + c * 10 + d
  }
}
console.log(level1(1)(2)(3))

// Functions of a block that change a variable, called from an arrow.
function tally(items: string[]) {
  let count = 0
  let text = ""
  function add(item: string) {
    count++
    text += item
  }
  items.forEach(item => add(item))
  return `${text}:${count}`
}
console.log(tally(["x", "y", "z"]))

// A global function value used by a function.
let current = (x: int) => x + 1
function useCurrent(x: int) {
  return current(x)
}
console.log(useCurrent(1))
current = x => x * 100
console.log(useCurrent(2))

// Block-scoped variables of top-level code.
if (n > 0) {
  const local = "local"
  let changed = 1
  const read = () => `${local}${changed}`
  changed = 2
  console.log(read())
}

// Distinct evaluations give distinct functions.
const made = [1, 2].map(() => () => 1)
console.log(made[0] == made[1], made[0] == made[0])
console.log(made, typeof made[0] == "function")

// Rest parameters through a function value.
const sum = (...xs: int[]) => xs.reduce((a, b) => a + b, 0)
const more = [4, 5]
console.log(sum(1, 2, 3), sum(...more), sum())

// A string captured and changed.
function builder() {
  let s = "start"
  return (x: string) => {
    s += x
    return s
  }
}
const append = builder()
append("-a")
console.log(append("-b"))
