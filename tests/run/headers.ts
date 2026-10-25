// same-as-node
// Headers: from a record, pairs or other Headers; names without case,
// values without the spaces around them; get joins the values of a name,
// set-cookie apart; iteration sorted by name; invalid names and values;
// how they print.
const h = new Headers({ "Content-Type": "text/plain", "X-A": "1" })
h.append("x-a", "2")
console.log(h)
console.log([...h], h.get("X-A"), h.get("none") ?? "(none)", h.has("content-TYPE"))
h.set("X-A", "3")
h.delete("content-type")
console.log(h, h.get("x-a"), h.has("X-a"))
const cookies = new Headers([["set-cookie", "a=1"], ["Set-Cookie", "b=2"], ["accept", " */* "]])
console.log(cookies.get("set-cookie"), cookies.getSetCookie(), cookies.get("accept"))
console.log([...cookies.keys()], [...cookies.values()], [...cookies.entries()])
cookies.forEach((value, name) => console.log(`${name}=${value}`))
console.log(new Headers(cookies).get("accept"), new Headers())
for (const bad of ["bad name", "", "ü"]) {
  try {
    new Headers().append(bad, "v")
  } catch (e) {
    console.log(e.name, e.message)
  }
}
try {
  new Headers().set("x", "a\nb")
} catch (e) {
  console.log(e.name, e.message)
}
