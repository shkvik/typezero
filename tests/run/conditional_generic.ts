// Conditional types in generic code (docs/generics.md, step 5): over type
// parameters a conditional type waits, and each copy resolves it with its
// types. The body may give a value of either branch; the copy checks the one
// its types choose, and drops the code after an `if` they decide.
type Boxed<T> = T extends string ? { text: T } : { value: T }
function box<T>(x: T): Boxed<T> {
  if (typeof x == "string") return { text: x }
  return { value: x }
}
const word: { text: string } = box("word")
const seven: { value: int } = box(7)
console.log(word, seven, word.text.length + seven.value)

// A union argument goes over the members, as in TypeScript.
type Kind<T> = T extends string ? "text" : T extends int ? "number" : "other"
function kind<T>(x: T): Kind<T> {
  if (typeof x == "string") return "text"
  if (typeof x == "number") return "number"
  return "other"
}
const mixed: string | int = Math.random() < 2 ? "a" : 1
const k: "text" | "number" = kind(mixed)
console.log(kind("a"), kind(1), kind(true), k)

// Template literal types of type parameters: T is a string, or a literal
// given explicitly.
function tag<T extends string>(name: T): `<${T}>` { return `<${name}>` }
const b: "<b>" = tag<"b">("b")
const pi: `<${string}>` = tag(String(Math.PI).slice(0, 4))
console.log(b, pi)

// Tuples in the types of generic functions.
type Last<T> = T extends [...infer Rest, infer L] ? L : undefined
function last<T extends unknown[]>(...xs: T): Last<T> | undefined { return undefined }
const l: string | undefined = last(1, true, "x")
console.log(l)

// ReturnType and Parameters of a generic function: its type parameters are
// their constraints, or unknown; of a method with several variants, the last.
function wrap<T>(x: T): T[] { return [x] }
function measure<T extends string>(x: T): int { return x.length }
type Wrapped = ReturnType<typeof wrap>
type MeasureArgs = Parameters<typeof measure>
const wrapped: Wrapped = [1, "a", true]
const measureArgs: MeasureArgs = ["text"]
class Vec {
  constructor(public x: double, public y: double) {}
  add(v: Vec): Vec { return new Vec(this.x + v.x, this.y + v.y) }
  add(k: double): Vec { return new Vec(this.x + k, this.y + k) }
  length() { return Math.hypot(this.x, this.y) }
}
const v = new Vec(3, 4)
type Added = ReturnType<typeof v.add>
type Length = ReturnType<typeof v.length>
const added: Added = v.add(1)
const len: Length = v.length()
console.log(wrapped.length, measure(...measureArgs), added, len)

// infer in instances of generic classes (also up the chain) and of
// dictionaries.
class Box<T> { constructor(public value: T) {} }
class IntBox extends Box<int> {}
type Unbox<B> = B extends Box<infer U> ? U : never
type ValueOf<M> = M extends Map<string, infer V> ? V : never
const unboxed: Unbox<IntBox> = new IntBox(5).value
const counted: ValueOf<Map<string, int>> = 3
console.log(unboxed + counted)

// Awaited of what an async function returns.
async function load(): Promise<int> { return 42 }
type Loaded = Awaited<ReturnType<typeof load>>
async function main() {
  const n: Loaded = await load()
  console.log(n + 1)
}
await main()
