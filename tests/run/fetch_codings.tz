// same-as-node
// fetch decodes what the server compressed: gzip, deflate (with zlib's
// header or without, as some servers send it) and br, whole and as it
// streams.
import { serve } from "tz:http"

const data: Record<string, string> = {
  gzip: "H4sIAAAAAAAACstIzcnJ11FIzs8tKEotLk5NUSjPL8pJUVTIGJUYrhIAPdXVhPQBAAA=", deflate: "eJzLSM3JyddRSM7PLShKLS5OTVEozy/KSVFUyBiVGK4SAM1Stlk=", raw: "y0jNycnXUUjOzy0oSi0uTk1RKM8vyklRVMgYlRiuEgA=", br: "G/MBIIxUtb8GGWt7EyE5SCeUpYWEsrTmtRS4EgM=",
}
const server = serve({ port: 0, hostname: "127.0.0.1" }, req => {
  const coding = new URL(req.url).pathname.slice(1)
  const body = Buffer.from(data[coding] ?? "", "base64")
  return new Response(body, { headers: { "content-encoding": coding == "raw" ? "deflate" : coding } })
})
for (const coding of ["gzip", "deflate", "raw", "br"]) {
  const r = await fetch(`http://127.0.0.1:${server.port}/${coding}`)
  const text = await r.text()
  console.log(coding, r.headers.get("content-encoding"), text.length, text.slice(0, 25))
}
const r = await fetch(`http://127.0.0.1:${server.port}/br`)
let total = 0
const body = r.body
if (body != undefined) for await (const chunk of body) total += chunk.length
console.log("streamed", total)
await server.stop()
