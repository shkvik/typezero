// same-as-node
// console.log shows objects exactly as Node.js does (util.inspect): keys in
// the order the literal gives them, strings quoted, one line while it fits
// in 80 columns, [Object] beyond two levels of nesting.
console.log({ a: 1, b: "two", c: true, d: 4.5, e: -0.0, f: NaN, g: -Infinity })
const big: long = 9007199254740991
console.log({ big, small: -7 }, [{ x: 1 }], "text", { y: "it's" })

// Around the line length: the entries and their count decide.
console.log({ first: "aaaaaaaaaaaaaaaa", second: "bbbbbbbbbbbbbbbb", third: "ccccccccccc" })
console.log({ first: "aaaaaaaaaaaaaaaa", second: "bbbbbbbbbbbbbbbb", third: "cccccccccccc" })
console.log({ first: "aaaaaaaaaaaaaaaa", second: "bbbbbbbbbbbbbbbb", third: "ccccccccccccc" })
console.log({ k1: 1, k2: 2, k3: 3, k4: 4, k5: 5, k6: 6, k7: 7, k8: 8, k9: 9, k10: 10, k11: 11, k12: 12 })
console.log({ k1: 1, k2: 2, k3: 3, k4: 4, k5: 5, k6: 6, k7: 7, k8: 8, k9: 9, k10: 10, k11: 11, k12: 12, k13: 13 })

// Nesting: two levels are shown, deeper objects and arrays are [Object] and
// [Array], but empty ones are shown.
console.log({ a: { b: { c: { d: 1 } } } })
const none: int[] = []
console.log({ a: { b: { c: [1] } }, e: { f: {} }, g: { h: { i: none } } })
console.log([{ a: { b: { c: 1 } } }])
console.log({ list: [1, 2, 3], nested: [[[1, 2], [3]]], empty: none, nothing: {} })
const deep = { level: 1, child: { level: 2, child: { level: 3, child: { level: 4 } } } }
console.log(deep, deep.child, deep.child.child)

// Long values break the object over lines, with their own indentation.
const report = {
  title: "A fairly long title for the report that is printed below",
  author: { name: "Ann Lee", email: "ann@example.com", roles: ["admin", "editor", "viewer"] },
  pages: 120,
  draft: false,
}
console.log(report)
console.log({ report })
console.log([report, report])
const numbers: int[] = []
for (let i = 0; i < 30; i++) numbers.push(i * i)
console.log({ name: "squares", numbers })
const text = "first line of a long text that goes on\nsecond line of the same text\nthird"
console.log({ text }, [{ text }])

// Arrays of objects: short ones line up in columns like other entries.
const cells: { v: int }[] = []
for (let i = 0; i < 12; i++) cells.push({ v: i })
console.log(cells)
const pairs = [
  { k: "a", v: 1 },
  { k: "b", v: 2 },
  { k: "c", v: 3 },
  { k: "d", v: 4 },
  { k: "e", v: 5 },
  { k: "f", v: 6 },
  { k: "g", v: 7 },
]
console.log(pairs)

// Keys that are not plain identifiers are quoted.
console.log({ "content-type": "json", $id: 1, _x: 2, x1: 3, "1st": 4, имя: "Иван", "it's": 5, "": 6 })

// Functions are shown by name.
function named(): void {}
const callbacks = { named, arrow: () => 1, inner: { fn: (x: int) => x } }
console.log(callbacks)

// Optional fields that were never set are not there, as in JavaScript.
interface Item {
  id: int
  note?: string
  parent?: Item
}
const root: Item = { id: 1 }
const leaf: Item = { id: 2, parent: root, note: "leaf" }
console.log(root, leaf)

// An object that refers to itself is marked, as Node does.
interface Loop {
  name: string
  self?: Loop
  child?: Loop
}
const loop: Loop = { name: "loop" }
loop.self = loop
console.log(loop)
const parent: Loop = { name: "parent" }
parent.child = { name: "child", self: parent }
console.log(parent, [parent])
const ring: Loop[] = []
for (let i = 0; i < 5; i++) {
  const item: Loop = { name: `n${i}` }
  item.self = item
  ring.push(item)
}
console.log(ring)
loop.self = undefined
parent.child = undefined
for (const item of ring) item.self = undefined
