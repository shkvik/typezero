// same-as-node
// Destructuring objects: renaming, default values, the rest, nested
// patterns, in parameters, in for...of and in assignments.
interface Pos {
  x: int
  y: int
}
interface Sprite {
  name: string
  pos: Pos
  tags: string[]
  hp?: int
}
const hero: Sprite = { name: "hero", pos: { x: 3, y: 4 }, tags: ["a", "b"] }
const slime: Sprite = { name: "slime", pos: { x: 0, y: 1 }, tags: [], hp: 5 }

const { name, hp = 100 } = hero
const { name: other, hp: health = 100 } = slime
console.log(name, hp, other, health)

// The rest is a new object with the other fields.
const { pos, ...rest } = hero
console.log(pos, rest, Object.keys(rest))

// Nested patterns, with arrays too, and keys that are not identifiers.
const {
  pos: { x, y: top },
  tags: [firstTag, ...otherTags],
} = hero
console.log(x, top, firstTag, otherTags)
const headers = { "content-type": "text/plain", "x-id": 7 }
const { "content-type": contentType, "x-id": id } = headers
console.log(contentType, id)
const sprites = [hero, slime]
if (sprites.length >= 2) {
  const [{ name: firstName }, { pos: second }] = sprites
  console.log(firstName, second)
}

// A default value is computed only when the field holds undefined.
let computed = 0
function fallback(): int {
  computed++
  return -1
}
const { hp: a = fallback() } = hero
const { hp: b = fallback() } = slime
console.log(a, b, computed)

// In parameters, of functions and arrows; the variables can be assigned to.
function describe({ name, pos: { x, y } }: Sprite, [mark, count]: [string, int]) {
  x += count
  return `${name} at ${x},${y} ${mark}`
}
console.log(describe(hero, ["#", 10]))
const area = ({ x, y }: Pos) => x * y
console.log(area({ x: 6, y: 7 }), sprites.map(({ pos: { x, y } }) => x + y))
console.log(sprites.filter(({ hp }) => hp != undefined).map(({ name }) => name))

// In for...of.
for (const { name, hp = 0 } of sprites) console.log(name, hp)
const pairs = [
  { key: "a", value: 1 },
  { key: "b", value: 2 },
]
let total = 0
for (const { value } of pairs) total += value
console.log(total)

// In assignments, to variables, fields and elements.
let px = 0
let py = 0
;({ x: px, y: py } = hero.pos)
console.log(px, py)
const target = { x: 0, y: 0 }
const cells = [0, 0]
;({ x: target.x, y: cells[1] } = { x: 8, y: 9 })
console.log(target, cells)
let label = ""
let leftover: { pos: Pos; tags: string[]; hp?: int } = { pos: { x: 0, y: 0 }, tags: [] }
;({ name: label, ...leftover } = slime)
console.log(label, leftover)
let level = 0
;({ hp: level = 1 } = hero)
console.log(level)
;[{ x: px }, py] = [{ x: 11, y: 0 }, 12]
console.log(px, py)
