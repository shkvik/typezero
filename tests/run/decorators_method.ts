// A decorator of a method gets its descriptor, MethodDescriptor<A, R>, and
// may replace what the method does: every call goes through what it leaves
// in `value`. Each use is a copy of the decorator for the method's types.

export function Log<A extends unknown[], R>(_cls: object, name: string, method: MethodDescriptor<A, R>) {
  const original = method.value
  method.value = function (this: unknown, ...args: A): R {
    const result = original.apply(this, args)
    console.log(`${name}(${args.join(", ")}) → ${String(result)}`)
    return result
  }
}

// Counts the calls, and gives the descriptor back, as TypeScript code often
// does.
const calls: Record<string, int> = {}
function Counted<A extends unknown[], R>(_cls: object, name: string, method: MethodDescriptor<A, R>) {
  const original = method.value
  method.value = function (this: unknown, ...args: A): R {
    calls[name] = (calls[name] ?? 0) + 1
    return original.call(this, ...args)
  }
  return method
}

// Replaces the method entirely: the arguments are those of the method.
function Twice(_cls: object, _name: string, method: MethodDescriptor<[int], int>) {
  const original = method.value
  method.value = function (this: unknown, n: int): int {
    return original.call(this, original.call(this, n))
  }
}

// Looks at the descriptor and leaves it as it is.
function Describe<A extends unknown[], R>(_cls: object, name: string, method: MethodDescriptor<A, R>) {
  console.log(name, method.writable, method.enumerable, method.configurable)
}

class Calc {
  constructor(public base: int) {}

  @Log
  add(a: int, b: int) {
    return a + b + this.base
  }

  @Log @Counted
  hello(who: string, punctuation = "!") {
    return `hello ${who}${punctuation}`
  }

  @Twice
  inc(n: int) {
    return n + this.base
  }

  @Describe @Counted
  reset() {
    this.base = 0
  }

  @Log
  static twice(x: double) {
    return x * 2
  }

  // Calls through `this` go through the decorators too.
  @Counted
  sum(xs: int[]): int {
    if (xs.length == 0)
      return 0
    return xs[0] + this.sum(xs.slice(1))
  }
}

const c = new Calc(10)
console.log(c.add(1, 2))
console.log(c.hello("Ann"), c.hello("Bob", "?"))
console.log(c.inc(1))
console.log(Calc.twice(2.5))
// A method as a value, bound to its object, runs what the decorators left.
const add = c.add
console.log(add(3, 4))
console.log([1, 2].map(n => c.inc(n)))
console.log(c.sum([1, 2, 3]))
c.reset()
console.log(c.add(1, 1), calls)

// A class that extends it: an override replaces the decorated method, and
// `super` calls it, decorators and all.
class Fancy extends Calc {
  override add(a: int, b: int) {
    return super.add(a, b) * 100
  }
}
const f = new Fancy(1)
console.log(f.add(1, 1))
const base: Calc = f
console.log(base.add(2, 2), base.inc(5))

// A generator method.
function Traced<A extends unknown[], R>(_cls: object, name: string, method: MethodDescriptor<A, R>) {
  const original = method.value
  method.value = function (this: unknown, ...args: A): R {
    console.log("start", name)
    return original.apply(this, args)
  }
}
class Range {
  @Traced
  *upTo(n: int) {
    for (let i = 0; i < n; i++) yield i
  }
}
console.log([...new Range().upTo(3)])
