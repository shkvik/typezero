// The parts of process, console, Date and performance that are not there
// (yet), and their misuse.

// expect-error: type 'int' is not assignable to type 'string'
process.env.HOME = 5
// expect-error: 'process.env.PATH' may be undefined, as a key may be missing: give it a default with '??', e.g. 'process.env.PATH = (process.env.PATH ?? "") + s'
process.env.PATH += ";x"
// expect-error: 'process.pid' is not supported yet
console.log(process.pid)
// expect-error: 'process.argv' cannot be assigned to
process.argv = ["a"]
// expect-error: only '=' can set 'process.exitCode'
process.exitCode += 1
// expect-error: type 'double' is not assignable to type 'int'; drop the fraction
process.exit(1.5)
// expect-error: type 'string' is not assignable to type 'int'
process.exitCode = "1"
// expect-error: expected 0-1 arguments, but got 2
process.exit(1, 2)
// expect-error: 'process.cwd' can only be called: 'process.cwd()'
const cwd = process.cwd
// expect-error: 'process.argv' is not a function
process.argv()
// expect-error: 'process.on' is not supported yet
process.on("exit", () => {})
// expect-error: 'process' can only be used through its properties, e.g. 'process.argv'
const p = process
// expect-error: type 'int' is not assignable to type 'string'
process.stdout.write(5)
// expect-error: type 'int' is not assignable to type 'string'
process.stderr.write(5)
// expect-error: 'process.stdout.isTTY' is a boolean, not a function
process.stdout.isTTY()
// expect-error: 'process.stdout.columns' is not supported yet; only 'write(text)' and 'isTTY' are
console.log(process.stdout.columns)
// expect-error: 'process.stdout' can only be used as 'process.stdout.write(text)'
const out = process.stdout
// expect-error: 'console.table' is not supported yet; only 'console.log', 'info', 'debug', 'error' and 'warn' are
console.table([1, 2])
// expect-error: an expression of type 'void' cannot be printed
console.error(console.log())
// expect-error: a date without a time is read as midnight UTC
Date.parse("2026-10-24")
// expect-error: 'Date.now' can only be called: 'Date.now()'
const now = Date.now
// expect-error: 'performance.mark' is not supported yet; only 'performance.now()' is
performance.mark("x")
// expect-error: type 'double' is not assignable to type 'int'
const ms: int = Date.now()
// expect-error: the value may be undefined; check it before reading 'length'
console.log(process.env.HOME.length)
