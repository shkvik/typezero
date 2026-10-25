// A small DI container whose classes are known when compiling: `make(Handler)`
// is a copy of `make` for Handler, where `Reflect.construct(cls, [])` is
// `new Handler()`, and in the copy of `invoke` for Handler, Reflect.get of the
// method by its name and Reflect.apply of it are a call of `handle`
// (docs/decorators.md, plan step 5). The exe holds no description of Handler.
// 5 million objects, two calls each. `di_reflect` is the same program with the
// class known only when it runs.
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

let sum = 0.0
for (let i = 0; i < 5000000; i++) {
  const h = make(Handler)
  invoke(h, i & 7)
  const r = invoke(h, 1)
  if (typeof r == "number") sum += r
}
console.log(sum)
