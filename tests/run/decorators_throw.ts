// expect-exit: 1
// expect-stderr: Error: cannot decorate
// What a decorated method throws goes up through its stub as from any
// method; the descriptor's first value checks the `this` it is called with.

function Check<A extends unknown[], R>(_cls: object, name: string, method: MethodDescriptor<A, R>) {
  const original = method.value
  method.value = function (this: unknown, ...args: A): R {
    if (args.length == 0) throw new RangeError(`${name} needs arguments`)
    return original.apply(this, args)
  }
}

class Box {
  constructor(public v: int) {}
  @Check
  get0() {
    return this.v
  }
  @Check
  fail(n: int) {
    if (n < 0) throw new TypeError("negative")
    return n * this.v
  }
}

try {
  new Box(1).get0()
} catch (e) {
  console.log(e.name, e.message)
}
try {
  new Box(1).fail(-1)
} catch (e) {
  console.log(e.name, e.message)
}
console.log(new Box(3).fail(7))

// Calls the method's own code with another `this`.
function Steal<A extends unknown[], R>(_cls: object, _name: string, method: MethodDescriptor<A, R>) {
  const original = method.value
  method.value = function (this: unknown, ...args: A): R {
    return original.apply("not a box", args)
  }
}
class Other {
  @Steal
  m() {
    return 1
  }
}
try {
  new Other().m()
} catch (e) {
  console.log(e.name, e.message)
}

// A decorator that throws stops the declaration of the class.
function Broken(cls: object) {
  console.log("decorating", cls)
  throw new Error("cannot decorate")
}
try {
  run()
} catch (e) {
  console.log(e.message)
}
function run() {
  console.log("before")
}
@Broken
class Late {}
console.log("not reached")
