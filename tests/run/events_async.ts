// same-as-node
// Async listeners (docs/events.md, section 3): emit does not wait for them,
// as in node; with captureRejections, the rejection of one becomes an
// 'error' event, on the next tick.
import { EventEmitter } from "node:events"

const sleep = (ms: int) => new Promise<void>((resolve) => setTimeout(resolve, ms))
const saved: string[] = []
async function save(name: string) {
  await sleep(1)
  saved.push(name)
}

const bus = new EventEmitter<{ user: [name: string] }>()
bus.on("user", async (name) => {
  await save(name)
  console.log("saved", name)
})
bus.on("user", save)
console.log(bus.emit("user", "ann"), saved, bus.listeners("user").length)
await sleep(5)
console.log(saved)
bus.off("user", save)
console.log(bus.listenerCount("user"))

const capture = new EventEmitter<{ job: [id: int]; error: [err: Error] }>({ captureRejections: true })
capture.on("job", async (id) => {
  await undefined
  console.log("job", id)
})
capture.once("job", async (id) => {
  throw new Error(`job ${id} failed`)
})
capture.on("error", (err) => console.log("error event:", err.message))
capture.emit("job", 7)
console.log("emitted", capture)
