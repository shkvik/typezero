// same-as-node
// AbortController and AbortSignal: abort() with a reason or without (an
// AbortError then), the listener `onabort` in the order it came,
// throwIfAborted(), AbortSignal.abort(), timeout() and any(), and how they
// print.
const log = (s: string) => console.log(s)
// The reason is any value; these show an Error by its message and name.
const message = (r: unknown) => (r instanceof Error ? r.message : "-")
const named = (r: unknown) => (r instanceof Error ? r.name : "-")
const controller = new AbortController()
const signal = controller.signal
console.log(controller, signal.aborted, signal.reason)
signal.onabort = () => log(`onabort: ${message(signal.reason)}`)
signal.throwIfAborted()
controller.abort(new Error("stop"))
controller.abort(new Error("again")) // nothing more happens
console.log(signal, signal.aborted, message(signal.reason))
try {
  signal.throwIfAborted()
} catch (e) {
  log(`thrown: ${e.message}`)
}

// Without a reason: an Error named AbortError.
const plain = new AbortController()
plain.abort()
log(`${named(plain.signal.reason)}: ${message(plain.signal.reason)}`)
const given = AbortSignal.abort()
log(`${given.aborted} ${named(given.reason)}`)
log(`${message(AbortSignal.abort(new Error("why")).reason)}`)

// onabort set to nothing does nothing; set again, it keeps its first place.
const quiet = new AbortController()
quiet.signal.onabort = () => log("not called")
quiet.signal.onabort = undefined
quiet.abort()

// any(): aborted with the reason of the first of its signals.
const a = new AbortController(), b = new AbortController()
const either = AbortSignal.any([a.signal, b.signal])
either.onabort = () => log(`either: ${message(either.reason)}`)
b.signal.onabort = () => log("b first")
b.abort(new Error("from b"))
a.abort(new Error("from a"))
log(`${either.aborted} ${message(either.reason)}`)
const early = AbortSignal.any([AbortSignal.abort(new Error("at once")), a.signal])
log(`${early.aborted} ${message(early.reason)} ${AbortSignal.any([]).aborted}`)

// timeout(): a TimeoutError once the time is up.
const t = AbortSignal.timeout(10)
t.onabort = () => log(`timeout: ${named(t.reason)} ${message(t.reason)}`)
await new Promise<void>((resolve) => setTimeout(resolve, 40))
// Its timer does not keep the program alive.
AbortSignal.timeout(100000).onabort = () => log("never")
log("end")
