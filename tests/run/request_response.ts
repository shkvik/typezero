// same-as-node
// Request and Response without the network: bodies of text, bytes, a
// stream, a form; text, json, bytes, arrayBuffer and formData once;
// clone; Response.json, redirect and error; the checks of the
// constructors; how they print.
const r = new Response("hello", { status: 201, headers: { "x-y": "z" } })
console.log(r)
const q = new Request("http://example.com/x", { method: "POST", body: "hi" })
console.log(q)
console.log(new ReadableStream())
console.log(await q.text(), q.bodyUsed)
try { await q.text() } catch (e) { console.log(e.name, e.message) }
const j = Response.json({ a: [1, 2] }, { status: 202 })
console.log(j.status, j.headers.get("content-type"), await j.json())
const red = Response.redirect("http://x.com/a", 307)
console.log(red.status, red.headers.get("location"))
try { red.headers.set("a", "b") } catch (e) { console.log(e.name, e.message) }
const e = Response.error()
console.log(e.status, e.type, e.ok)
try { new Response("x", { status: 99 }) } catch (err) { console.log(err.name, err.message) }
try { new Response("x", { status: 204 }) } catch (err) { console.log(err.name, err.message) }
try { new Request("http://x", { method: "GET", body: "x" }) } catch (err) { console.log(err.name, err.message) }
try { new Request("/relative") } catch (err) { console.log(err.name, err.message) }
const c = new Response("clone me")
const c2 = c.clone()
console.log(await c.text(), await c2.text())
const s = new Response(new ReadableStream({ start: ctl => { ctl.enqueue(new TextEncoder().encode("st")); ctl.close() } }))
const s2 = s.clone()
console.log(await s.text(), await s2.text())
console.log(new Request("http://h/", { method: "post" }).method)
const form = new URLSearchParams({ a: "1", b: "two words" })
const fr = new Response(form)
console.log(fr.headers.get("content-type"), await fr.text())
const fd = await new Response("a=1&b=2", { headers: { "content-type": "application/x-www-form-urlencoded" } }).formData()
console.log(fd.get("a"), fd.getAll("b"), fd.has("c"))
const ab = await new Response("abc").arrayBuffer()
console.log(ab.byteLength)
const b = await new Response(new Uint8Array([1, 2, 3])).bytes()
console.log(b)
