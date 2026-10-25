// Where tz differs from Node: a program has no script, so process.argv[1] is
// the executable, as process.argv[0] is; isTTY is false, not undefined, when
// the stream is not a terminal.
console.log(process.argv[0] == process.execPath, process.argv[1] == process.execPath)
console.log(process.argv.length)
console.log(process.stdout.isTTY, process.stderr.isTTY, typeof process.stdout.isTTY)
console.log(process.platform)
// As in Node, process.argv is one array: a change shows in the next read.
const argv = process.argv
argv.push("added")
console.log(process.argv.length, process.argv.at(-1))
process.argv.pop()
console.log(argv.length)
