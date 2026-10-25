// Unions: a million shapes of a discriminated union of four object types,
// mixed at random, and 20 passes that switch on the tag to sum their areas
// and perimeters; then a million tokens of `int | string` and 20 passes that
// tell them apart with typeof (in Go: a type switch over structs behind an
// interface, and over `any`).
interface Circle { kind: "circle"; r: double }
interface Rect { kind: "rect"; w: double; h: double }
interface Square { kind: "square"; side: double }
interface Triangle { kind: "triangle"; base: double; height: double; side: double }
type Shape = Circle | Rect | Square | Triangle

function area(s: Shape) {
  switch (s.kind) {
    case "circle": return 3.141592653589793 * s.r * s.r
    case "rect": return s.w * s.h
    case "square": return s.side * s.side
    case "triangle": return 0.5 * s.base * s.height
  }
}

function perimeter(s: Shape) {
  switch (s.kind) {
    case "circle": return 2 * 3.141592653589793 * s.r
    case "rect": return 2 * (s.w + s.h)
    case "square": return 4 * s.side
    case "triangle": return s.base + s.height + s.side
  }
}

const shapes: Shape[] = []
const tokens: (int | string)[] = []
let seed = 12345
for (let i = 0; i < 1000000; i++) {
  seed = (seed * 1103515245 + 12345) & 0x7fffffff
  const size = 1 + (seed % 100) / 10.0
  const kind = (seed >> 8) & 3
  if (kind == 0) shapes.push({ kind: "circle", r: size })
  else if (kind == 1) shapes.push({ kind: "rect", w: size, h: size + 1 })
  else if (kind == 2) shapes.push({ kind: "square", side: size })
  else shapes.push({ kind: "triangle", base: size, height: size + 2, side: size + 3 })
  if ((seed & 1) == 0) tokens.push(seed % 1000)
  else tokens.push(kind == 0 ? "a" : kind == 1 ? "bb" : "ccc")
}

let a = 0.0
let p = 0.0
let t = 0
for (let pass = 0; pass < 20; pass++) {
  for (const s of shapes) {
    a += area(s)
    p += perimeter(s)
  }
  for (const x of tokens) t += typeof x == "string" ? x.length : x
}
console.log(a.toFixed(1), p.toFixed(1), t)
