// Reflection of a class known when compiling keeps the checks the compiler
// cannot prove, with the same TypeErrors as through the description
// (docs/decorators.md, plan step 5): an argument of another type, too many
// arguments, a `this` of another class, arguments known only when the
// program runs. What it proves becomes the call itself.

class Point {
  x: int
  y: int
  constructor(x: int, y: int) {
    this.x = x
    this.y = y
  }
  scale(k: int) {
    return new Point(this.x * k, this.y * k)
  }
  describe(prefix?: string) {
    return `${prefix ?? "point"} ${this.x},${this.y}`
  }
}

class Other {
  scale(k: int) {
    return k
  }
}

function attempt(what: string, f: () => unknown) {
  try {
    console.log(what, f())
  } catch (e) {
    console.log(what, String(e))
  }
}

const p = new Point(1, 2)
const scale = Reflect.get(p, "scale")
const describe = Reflect.get(p, "describe")
attempt("proved", () => Reflect.apply(scale, p, [3]))
attempt("optional left out", () => Reflect.apply(describe, p, []))
attempt("optional given", () => Reflect.apply(describe, p, ["at"]))
attempt("optional undefined", () => Reflect.apply(describe, p, [undefined]))
attempt("fraction", () => Reflect.apply(scale, p, [2.5]))
attempt("string", () => Reflect.apply(scale, p, ["2"]))
attempt("too many", () => Reflect.apply(scale, p, [1, 2]))
attempt("too few", () => Reflect.apply(scale, p, []))
attempt("other this", () => Reflect.apply(scale, new Other(), [2]))
const k: unknown = 4
attempt("unknown argument", () => Reflect.apply(scale, p, [k]))
const n = 2.0
attempt("double argument", () => Reflect.apply(scale, p, [n]))

// Reflect.construct of the class with arguments known only when it runs.
function make(args: unknown[]): Point {
  return Reflect.construct(Point, args)
}
attempt("construct", () => make([5, 6]))
attempt("construct fraction", () => make([5, 6.5]))
attempt("construct arity", () => make([5]))
