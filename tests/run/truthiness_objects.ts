// same-as-node
// Truthiness of objects (docs/small.md, section 1): an object, an instance of
// a class, a class itself, a function or a dictionary that may be undefined
// is tested for being there, and narrows; `filter(Boolean)` drops undefined
// from the type of the elements; `unknown` is tested once a check shows what
// it holds. tests/errors/truthiness_objects.ts has what is always true.

class User {
  name: string
  email: string
  constructor(name: string, email: string) {
    this.name = name
    this.email = email
  }
}
function findUser(id: int): User | undefined {
  return id == 1 ? new User("ann", "a@x") : undefined
}
for (const id of [1, 2]) {
  const user = findUser(id)
  if (user) console.log(user.email)
  else console.log("none")
  console.log(!user, user ? user.name : "-", Boolean(user), user && user.name, user || new User("x", "y"))
}

// An object literal type, a dictionary and a function.
type Point = { x: int; y: int }
function pt(ok: boolean): Point | undefined {
  return ok ? { x: 1, y: 2 } : undefined
}
const p = pt(true)
if (p) console.log(p.x + p.y)
console.log(pt(false) ? "q" : "no q", pt(false) || { x: 0, y: 0 })
let cache: Map<string, int> | undefined
cache ||= new Map()
cache.set("a", 1)
console.log(cache.size)
let callback: ((x: int) => int) | undefined
console.log(callback ? "set" : "unset")
callback ??= x => x + 1
console.log(callback(1))

// A class itself, known when compiling or not, that may be undefined.
function pick(k: boolean): typeof User | undefined {
  return k ? User : undefined
}
const Kind = pick(true)
if (Kind) console.log(new Kind("b", "b@x").name)
const NoKind: Constructor<User> | undefined = pick(false)
console.log(NoKind ? "a class" : "no class", NoKind && NoKind.name, !Kind)

// A linked list: `while (node)` walks it.
class ListNode {
  value: int
  next?: ListNode
  constructor(value: int, next?: ListNode) {
    this.value = value
    this.next = next
  }
}
let node: ListNode | undefined = new ListNode(1, new ListNode(2, new ListNode(3)))
let sum = 0
while (node) {
  sum += node.value
  node = node.next
}
console.log(sum)

// filter(Boolean) keeps those that are there, and the type says so.
const users = [findUser(1), findUser(2), findUser(1)]
const present = users.filter(Boolean)
console.log(present.length, present.map(u => u.name))
console.log([pt(true), pt(false)].filter(Boolean).map(v => v.x))
console.log([pick(true), pick(false)].filter(Boolean).map(k => new k("z", "z").name))
const fns = [() => 1, undefined, () => 2]
console.log(fns.filter(Boolean).map(f => f()))

// A type parameter that is an object.
function nameOf<T extends { name: string }>(x: T | undefined): string {
  return x ? x.name : "?"
}
function firstOf<T extends object>(xs: (T | undefined)[]): T | undefined {
  for (const x of xs) if (x) return x
  return undefined
}
console.log(nameOf(findUser(1)), nameOf(findUser(2)), firstOf([undefined, new User("c", "c@x")])?.name)

// unknown, once a check shows what it holds.
function describe(v: unknown): string {
  if (v == undefined) return "nothing"
  if (typeof v == "string") return v ? `text ${v}` : "empty text"
  if (typeof v == "object") return "object"
  return typeof v
}
console.log(describe(undefined), describe(""), describe("t"), describe(new User("d", "d")), describe(0))
