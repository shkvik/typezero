// same-as-node
// A value that may be undefined, made a string by a template or `+`, is
// "undefined" when it holds nothing, as String(x) gives, also when it is a
// variable that the template only borrows.
function pick(n: int): string | undefined {
  return n > 0 ? "yes" : undefined
}
const s = pick(0)
const t = pick(1)
console.log(`${s}`, `[${s}]`, `[${t}]`, String(s), "a" + s, s + "b")
const n: int | undefined = pick(1) == undefined ? 1 : undefined
console.log(`${n}`, `[${n}]`)
const parts: (string | undefined)[] = ["a", undefined]
console.log(`${parts[1]}|${parts[0]}`, parts.join("-"))
let acc = ""
acc += s
console.log(acc)
const o: { name?: string; tag?: string } = { tag: "t" }
console.log(`<${o.name}> <${o.tag}>`, "n=" + o.name)
