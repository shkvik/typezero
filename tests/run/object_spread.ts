// same-as-node
// Spread in object literals: a new object with the fields of its parts.
interface User {
  id: int
  name: string
  email: string
  limit?: int
}
const user: User = { id: 1, name: "Ann", email: "ann@example.com" }

// The type is the resulting set of fields: here User again, or one more.
const renamed = { ...user, name: "Anna" }
const withRole = { ...user, role: "admin" }
const again: User = renamed
console.log(renamed, withRole, again.name, withRole.role)

// A new object: changing it leaves the old one as it was, but the objects
// its fields hold are shared, as in JavaScript.
interface Pos {
  x: int
  y: int
}
const sprite = { name: "hero", pos: { x: 3, y: 4 } }
const copy = { ...sprite }
copy.name = "copy"
copy.pos.x = 30
console.log(sprite, copy, copy == sprite, copy.pos == sprite.pos)

// The keys come in the order the parts first give them; a later part
// replaces the value, not the place.
let calls = 0
function next(): int {
  calls++
  return calls
}
const base = { a: 1, b: "x", c: true }
console.log({ z: next(), ...base, a: next(), d: [1] }, calls)
console.log({ a: "replaced", ...base }, { ...base, ...{ a: 2.5 } })
console.log({ ...{} }, { ...base, ...base })

// Optional fields: a part that has a value replaces the default.
interface Options {
  size?: double
  color?: string
}
const chosen: Options = { color: "red" }
const merged = { ...{ size: 1.5, color: "black" }, ...chosen }
console.log(merged, merged.size / 2, merged.color.toUpperCase())
const loose = { ...chosen, extra: 1 }
console.log(loose, loose.size ?? 0)

// Spreads in loops, with fields that hold arrays, objects and functions.
const items: { name: string; tags: string[]; owner: Pos; describe: () => string }[] = []
for (let i = 0; i < 3; i++) {
  const plain = { name: `item${i}`, tags: [`t${i}`], owner: { x: i, y: -i } }
  items.push({ ...plain, describe: () => `${plain.name} of ${plain.owner.x}` })
}
console.log(items.map(it => it.describe()), items[2]?.tags)
const updated = items.map(it => ({ ...it, tags: [...it.tags, "new"] }))
console.log(updated.map(it => it.tags.join("+")))
