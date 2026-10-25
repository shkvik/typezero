// Overloads with decorators and reflection (docs/overloads.md, section 6).
// A decorator on a variant gets that variant, and 'design:paramtypes' are its
// parameters. Reflect.get of a method with several variants gives a
// function that chooses the variant by the arguments when it is called, as
// a call that compiles chooses by their types: each argument fits it no
// worse, and one better (an int is an int before it is a double, an object
// of a class before one of its base); none, or two equally good, is a
// TypeError. `new` of a class known only when the program runs and
// Reflect.construct choose among its constructors the same way. node runs
// such code wrongly (the last body wins), so this test is tz's only.

function Log<A extends unknown[], R>(_cls: object, name: string, method: MethodDescriptor<A, R>) {
  const original = method.value
  method.value = function (this: unknown, ...args: A): R {
    const result = original.apply(this, args)
    console.log(`  ${name}(${args.join(", ")}) was called`)
    return result
  }
}
function Inject(token: string) {
  return (_cls: object, _name: string | undefined, index: int) => {
    console.log(`inject ${token} at ${index}`)
  }
}

class Animal {
  name: string
  constructor(name: string) {
    this.name = name
  }
}
class Dog extends Animal {}

class Vec {
  x: double
  y: double
  constructor(x: double, y: double) {
    this.x = x
    this.y = y
  }
  constructor(@Inject("origin") p: Vec) {
    this.x = p.x
    this.y = p.y
  }
  constructor(k: int) {
    this.x = k
    this.y = -k
  }
  add(v: Vec) {
    return new Vec(this.x + v.x, this.y + v.y)
  }
  @Log
  add(k: double) {
    return new Vec(this.x + k, this.y + k)
  }
  add(k: int) {
    return new Vec(this.x + k * 10, this.y + k * 10)
  }
  label(a: int, b: double) {
    return `int, double: ${a} ${b}`
  }
  label(a: double, b: int) {
    return `double, int: ${a} ${b}`
  }
  pet(a: Animal) {
    return `animal ${a.name}`
  }
  pet(d: Dog) {
    return `dog ${d.name}`
  }
  sum(...xs: int[]) {
    return `ints ${xs.length}`
  }
  sum(a: double, b: double) {
    return `two doubles ${a + b}`
  }
}

function attempt(what: string, f: () => unknown) {
  try {
    console.log(what, f())
  } catch (e) {
    console.log(what, String(e))
  }
}

// The decorated variant, and its records.
const v = new Vec(1, 1)
console.log(v.add(2), v.add(2.5), v.add(new Vec(1, 2)))
console.log(Reflect.getMetadata("design:paramtypes", Vec, "add"))
console.log(Reflect.getMetadata("design:paramtypes", Vec))

// Reflect.get of an object known only as an object: the variant is chosen
// when the function is called.
const o: object = v
const add = Reflect.get(o, "add")
console.log(add, typeof add)
attempt("add(Vec)", () => Reflect.apply(add, o, [new Vec(1, 1)]))
attempt("add(2)", () => Reflect.apply(add, o, [2]))
attempt("add(2.5)", () => Reflect.apply(add, o, [2.5]))
attempt("add(2.0)", () => Reflect.apply(add, o, [Math.sqrt(4)]))
attempt("add('s')", () => Reflect.apply(add, o, ["s"]))
attempt("add()", () => Reflect.apply(add, o, []))
const label = Reflect.get(o, "label")
attempt("label(1, 2)", () => Reflect.apply(label, o, [1, 2]))
attempt("label(1, 2.5)", () => Reflect.apply(label, o, [1, 2.5]))
attempt("label(1.5, 2)", () => Reflect.apply(label, o, [1.5, 2]))
const pet = Reflect.get(o, "pet")
attempt("pet(Dog)", () => Reflect.apply(pet, o, [new Dog("rex")]))
attempt("pet(Animal)", () => Reflect.apply(pet, o, [new Animal("cat")]))
const sum = Reflect.get(o, "sum")
attempt("sum(1, 2)", () => Reflect.apply(sum, o, [1, 2]))
attempt("sum(1.5, 2)", () => Reflect.apply(sum, o, [1.5, 2]))
attempt("sum(1, 2, 3)", () => Reflect.apply(sum, o, [1, 2, 3]))
attempt("sum()", () => Reflect.apply(sum, o, []))
attempt("this", () => Reflect.apply(add, new Dog("x"), [1]))

// Of an object of a class known when compiling: the same function, without
// the description of the class.
const known = Reflect.get(v, "add")
attempt("known add(3)", () => Reflect.apply(known, v, [3]))
attempt("known add(3.5)", () => Reflect.apply(known, v, [3.5]))

// The constructors of a class known only when the program runs.
const kinds: Constructor<Vec>[] = [Vec]
for (const kind of kinds) {
  attempt("new(1, 2)", () => new kind(1, 2))
  attempt("new(Vec)", () => new kind(new Vec(3, 4)))
  attempt("new(5)", () => new kind(5))
  attempt("new(5.5)", () => new kind(5.5))
  attempt("new('x')", () => new kind("x"))
  const args: unknown[] = [7]
  attempt("construct([7])", () => Reflect.construct(kind, args))
}
const fromArgs: unknown[] = [2, 3]
attempt("construct Vec", () => Reflect.construct(Vec, fromArgs))

// A class that adds a variant: its objects have both.
class Vec3 extends Vec {
  add(s: string) {
    return new Vec(this.x + s.length, this.y)
  }
}
const w: object = new Vec3(1, 2)
const add3 = Reflect.get(w, "add")
attempt("Vec3 add('abc')", () => Reflect.apply(add3, w, ["abc"]))
attempt("Vec3 add(1)", () => Reflect.apply(add3, w, [1]))
attempt("Vec add('abc')", () => Reflect.apply(add, o, ["abc"]))

// A rest parameter: the arguments it takes are compared too, and of two
// that fit as well, the variant without one is better.
class Pairs {
  pair(a: int, ...rest: int[]) {
    return `rest ${rest.length}`
  }
  pair(a: int, b: int) {
    return "two"
  }
}
const ps: object = new Pairs()
const pair = Reflect.get(ps, "pair")
attempt("pair(1, 2)", () => Reflect.apply(pair, ps, [1, 2]))
attempt("pair(1)", () => Reflect.apply(pair, ps, [1]))
attempt("pair(1, 2, 3)", () => Reflect.apply(pair, ps, [1, 2, 3]))
