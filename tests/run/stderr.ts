// same-as-node
// merge-stderr
// expect-exit: 1
// console.error and console.warn print to stderr as console.log prints to
// stdout. stdout is buffered, but it is written out before anything goes to
// stderr, so with `2>&1` the lines come in the order they were printed.
console.log("out 1")
console.error("err 1")
console.warn("warn:", 42, [1, 2], { a: "b" })
console.info("info", true)
console.debug("debug", undefined)
process.stdout.write("out 2, ")
process.stdout.write("still out 2\n")
process.stderr.write("err 2 without a newline; ")
console.error()
for (let i = 3; i <= 5; i++) {
  console.log(`out ${i}`)
  if (i == 4) console.error(`err ${i - 1}`)
}
console.log(process.stdout.write(""), process.stderr.write(""))
const print: (s: string) => void = console.error
print("err 4, through a function value")
const letters = ["x", "y"]
letters.forEach(s => console.warn(s))
// process.exit() without a code ends with process.exitCode.
process.exitCode = 1
process.exit()
