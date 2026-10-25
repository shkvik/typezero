// same-as-node
// Unions of objects: a reference, whose descriptor tells the member. Checks
// of the tag, `in` and `instanceof` narrow; a field every member has is read
// through the union.
interface Circle { kind: "circle"; r: double; label: string }
interface Square { kind: "square"; side: double; label: string }
interface Rect { kind: "rect"; w: double; h: double; label: string }
type Shape = Circle | Square | Rect

function area(s: Shape) {
  switch (s.kind) {
    case "circle": return Math.PI * s.r ** 2
    case "square": return s.side ** 2
    case "rect": return s.w * s.h
  }
}

function describe(s: Shape) {
  if (s.kind == "circle") return `circle of radius ${s.r}`
  if ("side" in s) return `square of side ${s.side}`
  return `rect ${s.w}x${s.h}`
}

const shapes: Shape[] = [
  { kind: "circle", r: 1, label: "a" },
  { kind: "square", side: 2, label: "b" },
  { kind: "rect", w: 2, h: 3, label: "c" },
]
for (const s of shapes) console.log(s.kind, s.label, area(s).toFixed(2), describe(s))
console.log(shapes)
console.log(JSON.stringify(shapes[1]), typeof shapes[0])

// A literal finds its member by its fields and its tag; one that goes where
// a union is expected later does too.
const ring = { kind: "circle", r: 2, label: "ring" }
console.log(area(ring), describe({ kind: "rect", w: 1, h: 1, label: "x" }))
shapes.push({ kind: "square", side: 1, label: "d" })
console.log(shapes.length, shapes[3])

// Writing a field of every member, and reading one narrowed by a check of a
// field it is in.
function relabel(s: Shape, label: string) {
  s.label = label
  return s
}
console.log(relabel(shapes[0] ?? ring, "first"))
interface Slot { name: string; shape: Shape }
function width(slot: Slot) {
  if (slot.shape.kind == "circle") return 2 * slot.shape.r
  if (slot.shape.kind == "square") return slot.shape.side
  return slot.shape.w
}
console.log(width({ name: "x", shape: { kind: "circle", r: 1.5, label: "" } }))

// Classes: instanceof narrows, a method of a class they all extend is
// called through it, and one of each otherwise.
class Animal {
  name: string
  constructor(name: string) {
    this.name = name
  }
  speak() { return "..." }
}
class Dog extends Animal {
  speak() { return "woof" }
  fetch() { return `${this.name} fetches` }
}
class Cat extends Animal {
  speak() { return "meow" }
}
class Robot {
  id: int
  constructor(id: int) {
    this.id = id
  }
  speak() { return `beep ${this.id}` }
}
function act(x: Dog | Cat | Robot) {
  if (x instanceof Dog) return x.fetch()
  if (x instanceof Robot) return x.speak()
  return x.name + " " + x.speak()
}
const crowd: (Dog | Cat | Robot)[] = [new Dog("rex"), new Cat("tom"), new Robot(7)]
console.log(crowd.map(act), crowd.map(x => x.speak()))
const pets: (Dog | Cat)[] = [new Cat("kit"), new Dog("max")]
console.log(pets.map(p => `${p.name}: ${p.speak()}`), pets)

// A tag of boolean literals, and narrowing by truth.
type Answer = { ok: true; value: int } | { ok: false; error: string }
function show(a: Answer) {
  if (a.ok) return `value ${a.value}`
  return `error ${a.error}`
}
console.log(show({ ok: true, value: 42 }), show({ ok: false, error: "nope" }))

// Filter with a check of the tag keeps the members it shows.
const circles = shapes.filter(s => s.kind == "circle")
console.log(circles.map(c => c.r))

// An assignment narrows; a loop that assigns forgets it.
let current: Shape = { kind: "square", side: 3, label: "s" }
console.log(current.side)
let sum = 0.0
for (let i = 0; i < 4; i++) {
  if (current.kind == "square") sum += current.side
  current = i % 2 == 0 ? { kind: "circle", r: 1, label: "c" } : { kind: "square", side: 2, label: "q" }
}
console.log(sum)

// `?.` of a union that may be undefined: a check of its tag shows it is there.
function sideOf(s: Shape | undefined) {
  if (s?.kind == "circle") return s.r
  if (s?.kind != "square") return -1
  return s.side
}
console.log(sideOf(undefined), sideOf({ kind: "circle", r: 2, label: "" }), sideOf(shapes[1] ?? ring))
