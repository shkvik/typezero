// `di_known` with the class known only when the program runs: `make(kind)`
// of a `Constructor` makes the object through the description of the class,
// checking the arguments, and Reflect.get finds the method by its name in the
// description, whose function checks its `this`, and Reflect.apply checks the
// arguments against its parameters (docs/decorators.md, section 6).
class Handler {
  hits = 0
  handle(n: int) {
    this.hits += n
    return this.hits
  }
}

function make<T extends object>(cls: Constructor<T>): T {
  return Reflect.construct(cls, [])
}

function invoke<T extends object>(obj: T, n: int): unknown {
  const method = Reflect.get(obj, "handle")
  if (typeof method != "function") return undefined
  return Reflect.apply(method, obj, [n])
}

const kinds: Constructor[] = [Handler]
let sum = 0.0
for (const kind of kinds) {
  for (let i = 0; i < 5000000; i++) {
    const h = make(kind)
    invoke(h, i & 7)
    const r = invoke(h, 1)
    if (typeof r == "number") sum += r
  }
}
console.log(sum)
