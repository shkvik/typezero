// Misuses of node:fs/promises, node:timers/promises, node:readline,
// AbortController and AbortSignal.
import { readFile, stat, open, writeFile } from "node:fs/promises"
import { setTimeout as wait } from "node:timers/promises"
import { createInterface } from "node:readline/promises"

const options = { encoding: "utf8" }
// Fine: without an encoding, a Promise<Buffer>.
const bytes = await readFile("a.txt")
// expect-error: an encoding is for text; 'writeFile' writes the bytes as they are: drop the encoding
await writeFile("b.txt", bytes, "hex")
// expect-error: 'stat' has no option 'throwIfNoEntry'; it takes bigint
await stat("a.txt", { throwIfNoEntry: false })
// expect-error: the options of 'readFile' are written in the call, as an object literal
await readFile("a.txt", options)
// expect-error: 'open' of 'node:fs/promises' is not supported yet: there is no FileHandle
await open("a.txt")
// expect-error: 'writeFile' writes a string or bytes (a Uint8Array or a Buffer); found 'int'
await writeFile("a.txt", 5)
// expect-error: 'setTimeout' has no option 'repeat'; it takes signal and ref
await wait(10, undefined, { repeat: true })
// expect-error: the Promise is not awaited
wait(10)

// expect-error: an AbortSignal is not made with 'new'
const made = new AbortSignal()
const controller = new AbortController()
// expect-error: 'aborted' of an AbortSignal is read-only; abort it with its controller, 'controller.abort(reason)'
controller.signal.aborted = true
// expect-error: 'reason' of an AbortSignal is read-only
controller.signal.reason = new Error("why")
// expect-error: only '=' can set 'onabort'
controller.signal.onabort ??= () => console.log("aborted")
// expect-error: class 'AbortSignal' has no static member 'never'
AbortSignal.never()
// expect-error: property 'cancel' does not exist on type 'AbortController'
controller.cancel()
// expect-error: 'AbortController' is not assignable
await wait(10, undefined, { signal: controller })

// expect-error: readline reads only stdin yet: the input is 'process.stdin'
const other = createInterface({ input: process.stdout })
// expect-error: the option 'prompt' of 'createInterface' is not supported yet
const prompted = createInterface({ input: process.stdin, prompt: "> " })
const rl = createInterface({ input: process.stdin })
// expect-error: 'on' of readline is not supported yet: its Interface is no EventEmitter yet (docs/events.md)
rl.on("line", (line: string) => console.log(line))
// expect-error: property 'line' does not exist on type 'Interface'
console.log(rl.line)
