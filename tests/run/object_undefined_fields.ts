// A field that is not there and one that holds undefined are the same, so
// an explicit undefined does not clobber a default in a spread, as it does in
// JavaScript, and Object.keys, for...in and destructuring's rest leave such
// a field out.
interface Query {
  text: string
  limit?: int
}
const a: Query = { text: "cats" }
const b: Query = { text: "dogs", limit: undefined }
const defaults = { limit: 20 }
const final = { ...defaults, ...b }
console.log(final.limit, final) // node: undefined { limit: undefined, text: 'dogs' }
console.log({ ...defaults, ...a }.limit + 1, { ...b, limit: 5 })
console.log(Object.keys(b), Object.keys({ ...b }).length) // node: [ 'text', 'limit' ] 2
for (const key in b) console.log(key) // node: text, limit
const { text, ...rest } = b
console.log(text, rest, Object.keys(rest)) // node: dogs { limit: undefined } [ 'limit' ]

// A later explicit undefined still replaces the value.
const cleared = { ...defaults, limit: undefined }
console.log(cleared.limit ?? -1)
