// same-as-node
// ReadableStream, as the bodies of fetch have it: made with start and
// pull, sync or async; read with getReader() and with for await; cancel,
// an error of the source, locked; how it prints.
const enc = new TextEncoder()
const dec = new TextDecoder()

const words = ["alpha", "beta", "gamma"]
let next = 0
const pulled = new ReadableStream<string>({
  pull: async c => {
    await new Promise<void>(resolve => setTimeout(resolve, 5))
    const word = words[next++]
    if (word == undefined) c.close()
    else c.enqueue(word)
  },
})
console.log(pulled, pulled.locked)
const reader = pulled.getReader()
console.log(pulled.locked)
for (;;) {
  const r = await reader.read()
  if (r.done) break
  console.log("read", r.value)
}
reader.releaseLock()
console.log(pulled.locked)

const bytes = new ReadableStream({
  start: c => {
    c.enqueue(enc.encode("one "))
    c.enqueue(enc.encode("two"))
    c.close()
  },
})
let text = ""
for await (const chunk of bytes) text += dec.decode(chunk)
console.log(text)

const failing = new ReadableStream<int>({
  start: c => {
    c.enqueue(1)
    c.error(new Error("source failed"))
  },
})
try {
  for await (const n of failing) console.log("got", n)
} catch (e) {
  console.log("caught", e.message)
}

let cancelled = ""
const endless = new ReadableStream<int>({
  pull: c => {
    c.enqueue(7)
  },
  cancel: reason => {
    cancelled = reason?.message ?? "no reason"
  },
})
for await (const n of endless) {
  console.log("first", n)
  break
}
console.log("cancelled:", cancelled)
const other = new ReadableStream<int>({ cancel: reason => { cancelled = reason?.message ?? "" } })
await other.cancel(new Error("enough"))
console.log("cancelled:", cancelled, other)
try {
  const r = new ReadableStream()
  r.getReader()
  r.getReader()
} catch (e) {
  console.log(e.name, e.message)
}
