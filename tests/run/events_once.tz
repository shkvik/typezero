// same-as-node
// once(emitter, name) of node:events (docs/events.md, section 4): a Promise
// of the arguments of the next event, rejected by an 'error' event or when
// its signal aborts; also of an EventTarget. getEventListeners() and
// listenerCount() of the module, and the same functions on EventEmitter.
import { EventEmitter, once, getEventListeners, listenerCount } from "node:events"

const server = new EventEmitter<{ listening: [port: double, host: string]; error: [err: Error]; done: [] }>()
setTimeout(() => server.emit("listening", 8080, "localhost"), 1)
const [port, host] = await once(server, "listening")
console.log("listening", port, host, server.listenerCount("listening"), server.listenerCount("error"))

const failing = once(server, "done")
server.emit("error", new Error("broken"))
try {
  await failing
} catch (err) {
  console.log("rejected", err.message)
}
setTimeout(() => server.emit("done"), 1)
console.log(await EventEmitter.once(server, "done"))

const ac = new AbortController()
const waiting = once(server, "done", { signal: ac.signal })
ac.abort()
try {
  await waiting
} catch (err) {
  console.log(err.name, err.message, server.listenerCount("done"))
}
const signalled = new AbortController()
const fired = once(server, "done", { signal: signalled.signal })
server.emit("done")
console.log(await fired, server.listenerCount("done"))

server.on("done", () => {})
console.log(getEventListeners(server, "done").length, listenerCount(server, "done"))
console.log(EventEmitter.listenerCount(server, "done"))

// An EventTarget: the event, in an array.
const target = new EventTarget()
setTimeout(() => target.dispatchEvent(new Event("ping")), 1)
const [event] = await once(target, "ping")
console.log(event.type, getEventListeners(target, "ping").length)
