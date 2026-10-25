// same-as-node
// An async listener that throws, without captureRejections: an unhandled
// rejection, which ends the program as in node, after the code that
// emitted.
// expect-exit: 1
// expect-stderr: Error: lost
import { EventEmitter } from "node:events"

const bus = new EventEmitter<{ job: [] }>()
bus.on("job", async () => {
  throw new Error("lost")
})
bus.emit("job")
console.log("after emit")
