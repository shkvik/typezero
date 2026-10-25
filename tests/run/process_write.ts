// same-as-node
// merge-stderr
// process.stdout.write and process.stderr.write of text and of bytes, as
// they are, in order with console.log (no stream of node:stream here).
console.log("one")
process.stdout.write(new Uint8Array([116, 119, 111, 10]))
process.stderr.write(Buffer.from("to stderr\n"))
const ok = process.stdout.write(Buffer.from("три\n"))
console.log(ok, process.stdout.write(""))
const bytes = Buffer.from("a partial line")
process.stdout.write(bytes.subarray(2, 9))
process.stdout.write("\n")
