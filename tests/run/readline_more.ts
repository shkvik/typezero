// Lines of node:readline wait until they are asked for (readline_more.in):
// questions one after another each get the next line, as from a terminal,
// where Node drops the lines a file gives meanwhile (so this test is not
// compared with Node). A question with a callback, one cancelled by its
// signal, `for await` over the rest, and an Interface closed.
import { createInterface } from "node:readline/promises"
import { createInterface as withCallbacks } from "node:readline"
import { setTimeout as wait } from "node:timers/promises"

const rl = createInterface({ input: process.stdin, output: process.stdout })
const name = await rl.question("Name? ")
const age = await rl.question("Age? ")
console.log(`${name} is ${age}`)

// A question whose signal is aborted before the answer is read.
const controller = new AbortController()
controller.abort()
try {
  await rl.question("Never? ", { signal: controller.signal })
} catch (e) {
  console.log(`question: ${e.name}`)
}

// node:readline gives the answer to a callback.
const cb = withCallbacks({ input: process.stdin, output: process.stdout })
const answer = await new Promise<string>((resolve) => cb.question("City? ", (city) => resolve(city)))
console.log(`city ${answer}`)
cb.close()

// `for await` takes two lines and leaves; the Interface is closed then.
let taken = 0
for await (const line of rl) {
  console.log("line:", line)
  if (++taken == 2) break
}
try {
  await rl.question("Closed? ")
} catch (e) {
  console.log(`after close: ${e.message}`)
}
for await (const line of rl) console.log("never", line)

// A new Interface gets what is left.
const rest = createInterface({ input: process.stdin })
for await (const line of rest) console.log("rest:", line)
await wait(1)
console.log("end")
