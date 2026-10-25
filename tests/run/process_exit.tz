// same-as-node
// expect-exit: 3
// expect-stderr: usage: process_exit <file>
// process.exit() ends the program at once, with what is printed so far
// written out. The code after it never runs, so in fileArgument the array is
// known not to be empty after the check, and fail() needs no return.
function fileArgument(args: string[]): string {
  if (args.length == 0) {
    console.error("usage: process_exit <file>")
    process.exit(3)
  }
  return args[0]
}

function fail(message: string): int {
  console.error(message)
  process.exit(4)
}

process.exitCode = 9 // the code given to process.exit() wins
console.log("before")
const value = process.env.TZ_TEST_SURELY_MISSING
if (value == undefined) console.log("no value")
else console.log(fail(value))
process.stdout.write("a partial line")
const file = fileArgument(process.argv.slice(2))
console.log("never printed", file)
