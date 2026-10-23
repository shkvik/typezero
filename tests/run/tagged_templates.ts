// same-as-node
// Tagged templates: the tag gets the texts around the values as one array of
// strings, then the values, checked as in any call. String.raw keeps the
// texts as written.
function show(texts: string[], ...values: int[]): string {
  let out = ""
  for (let i = 0; i < texts.length; i++) {
    out += `[${texts[i]}]`
    const value = values[i]
    if (value != undefined) out += `<${value}>`
  }
  return out
}
const n = 5
console.log(show`a${n}b${n * 2}c`, show`plain`, show``, show`${1}${2}`)
console.log(show`tab\there, line\nbreak, \u0041, \${n}`)
console.log(show`first
second`)

// TypeScript's type for the texts is accepted; values may have any types.
function sql(parts: TemplateStringsArray, id: int, name: string): string {
  return `${parts.join("?")} -- ${id}, ${name}`
}
console.log(sql`select * from users where id = ${42} and name = ${"Ann"}`)

// A tag can be any function value, and the call is like any other.
const upper = (texts: string[], word: string) => texts.join(word.toUpperCase())
const lower = (texts: string[], word: string) => texts.join(word.toLowerCase())
console.log(upper`<${"mid"}>`)
for (const tag of [upper, lower]) console.log(tag`(${"Xy"})`)
let made = 0
function counted(texts: string[], value: int): int {
  made++
  return texts.length + value
}
for (let i = 0; i < 3; i++) counted`x${i}y`
console.log(made, counted`${made}`)

// String.raw: escapes stay as written, values are converted as in a template.
console.log(String.raw`C:\dir\new\t${n}\u0041`, String.raw`a\`b`, String.raw`\${n}`)
console.log(String.raw`line1
line2`.length, String.raw``, String.raw`${1.5}${true}`)
const path = String.raw`C:\Users\${"Ann"}\docs`
console.log(path, path.length)

// console.log as a tag prints the array of texts, then the values.
console.log`one ${n} two`
