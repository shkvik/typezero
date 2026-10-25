// e.stack, as in Node: the text of the error, then a line for each function
// the error passed on its way up, with the file, line and column of the
// call (docs/errors.md). The paths are cut to the name of the file here.
function show(stack: string) {
  for (const line of stack.split("\n")) {
    const slash = Math.max(line.lastIndexOf("/"), line.lastIndexOf("\\"))
    const open = line.indexOf("(")
    console.log(slash < 0 ? line : (open < 0 ? "    at " : line.slice(0, open + 1)) + line.slice(slash + 1))
  }
}

function level3(n: int): int {
  if (n > 2) throw new RangeError(`deep ${n}`)
  return n
}
function level2(n: int): int {
  return level3(n + 1) * 2
}
class Box {
  open(n: int) {
    return level2(n)
  }
}
try {
  console.log(new Box().open(1))
  console.log(new Box().open(5))
} catch (e) {
  show(e.stack)
}

// A built-in that fails is the first place.
const zero = [0][0] ?? 0
try {
  console.log(level2(10 / zero))
} catch (e) {
  show(e.stack)
}

// At most ten places, as Node's Error.stackTraceLimit.
function down(n: int): int {
  if (n == 0) throw new Error("bottom")
  return down(n - 1) + 1
}
try {
  down(20)
} catch (e) {
  console.log(e.stack.split("\n").length)
}

// An error that was not thrown has no places: its stack is its text, and
// console.log shows it in brackets, as Node shows an error without a stack.
const made = new Error("made", { cause: new TypeError("inner") })
console.log(made.stack)
console.log(made)
console.log([new RangeError("in an array")])
