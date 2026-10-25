// expect-stderr: RangeError: the body of the request is larger than maxBodySize
// How fetch fails, as in node ("fetch failed" with the cause), and where tz
// says more: a request larger than maxBodySize gets 413; data: URLs need
// no network. A name that is not found may be EAI_AGAIN on a machine
// without a network. https is in tls_serve.ts and tls_errors.ts.
import { serve } from "tz:http"

async function attempt(what: string, url: string, init?: RequestInit) {
  try {
    const r = await fetch(url, init)
    console.log(what, r.status, await r.text())
  } catch (e) {
    const cause = e.cause
    const code = cause instanceof SystemError ? ` [${cause.code == "EAI_AGAIN" ? "ENOTFOUND" : cause.code}]` : ""
    const text = cause == undefined ? "" : `(${cause.message.replaceAll("EAI_AGAIN", "ENOTFOUND")})`
    console.log(what, `${e.name}: ${e.message}${code}`, text)
  }
}

await attempt("bad port", "http://localhost:6000/")
await attempt("ftp", "ftp://example.com/")
await attempt("no url", "not a url")
await attempt("credentials", "http://user:pw@localhost/")
await attempt("no host", "http://no-such-host.invalid/")
await attempt("data", "data:text/plain,hello%20world")
await attempt("base64", "data:;base64,aGk=")
await attempt("get with body", "http://localhost:8000/", { method: "GET", body: "x" })
await attempt("no duplex", "http://localhost:8000/", {
  method: "POST",
  body: new ReadableStream({ start: c => { c.close() } }),
})

const corrupt = Buffer.from("not gzip at all")
const server = serve({ port: 0, hostname: "127.0.0.1", maxBodySize: 10 }, async req => {
  const path = new URL(req.url).pathname
  if (path == "/gzip") return new Response(corrupt, { headers: { "content-encoding": "gzip" } })
  return new Response(`${(await req.text()).length} bytes`)
})
const base = `http://127.0.0.1:${server.port}`
await attempt("small body", base, { method: "POST", body: "0123456789" })
await attempt("large body", base, { method: "POST", body: "0123456789!" })
const big = new ReadableStream({
  start: c => {
    c.enqueue(new TextEncoder().encode("0123456789"))
    c.enqueue(new TextEncoder().encode("!"))
    c.close()
  },
})
await attempt("large stream", base, { method: "POST", body: big, duplex: "half" })
await attempt("corrupt gzip", `${base}/gzip`)
const r = await fetch(base, { method: "POST", body: "once" })
console.log(await r.text(), r.bodyUsed)
try {
  await r.text()
} catch (e) {
  console.log(e.name, e.message)
}
await server.stop()
