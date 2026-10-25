// same-as-node
// The order of what an abort does, as in node: onabort in the place it was
// first set among the listeners (set to undefined, it keeps its place and
// does nothing); the signals of AbortSignal.any() aborted before the
// listeners of their source run; and the waits of the runtime that a signal
// cancels (node:timers/promises, node:fs/promises) rejected in the turns of
// the queue node takes, among the microtasks of the listeners.
import { setTimeout as wait } from "node:timers/promises"
import { readFile } from "node:fs/promises"

const log = (s: string) => console.log(s)

const c = new AbortController()
c.signal.addEventListener("abort", () => log("listener 1"))
c.signal.onabort = () => log("onabort")
c.signal.addEventListener("abort", () => log("listener 2"))
c.abort()

const g = new AbortController()
g.signal.onabort = () => log("first handler")
g.signal.addEventListener("abort", () => log("g listener"))
g.signal.onabort = undefined
g.signal.onabort = () => log("second handler")
g.abort("plain reason")
const reason = g.signal.reason
log(typeof reason == "string" ? reason : "?")

const a = new AbortController()
const any = AbortSignal.any([a.signal])
const anyOfAny = AbortSignal.any([any])
any.addEventListener("abort", () => log(`any listener ${a.signal.aborted} ${anyOfAny.aborted}`))
anyOfAny.addEventListener("abort", () => log("any of any listener"))
a.signal.addEventListener("abort", () => log(`a listener ${any.aborted} ${anyOfAny.aborted}`))
a.abort()

const d = new AbortController()
d.signal.addEventListener("abort", () => queueMicrotask(() => log("listener micro (before)")))
const p = wait(1000, "x", { signal: d.signal })
d.signal.addEventListener("abort", () => queueMicrotask(() => log("listener micro (after)")))
p.catch((e) => log(`rejected ${e.name} ${e.cause instanceof Error ? e.cause.name : "-"}`))
d.abort()
await wait(10)

const e = new AbortController()
const q = wait(1000, "x", { signal: e.signal })
q.then(undefined, (err) => log(`rejected 2 ${err.name}`))
e.signal.addEventListener("abort", () => log("listener sync"))
e.abort()
log("after abort")
await wait(10)

const f = new AbortController()
const r = readFile(process.argv[1] ?? "", { signal: f.signal })
r.then(undefined, (err) => log(`rejected 3 ${err.name}`))
f.signal.addEventListener("abort", () => queueMicrotask(() => log("listener micro 3")))
f.abort()
await wait(10)

// A listener that stops the others does not stop the runtime's.
const h = new AbortController()
h.signal.addEventListener("abort", (ev) => ev.stopImmediatePropagation())
h.signal.addEventListener("abort", () => log("not called"))
const s = wait(1000, "x", { signal: h.signal })
h.abort()
await s.catch((err) => log(`stopped anyway: ${err.name}`))
