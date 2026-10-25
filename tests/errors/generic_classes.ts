// Generic classes: errors in their bodies, checked once with opaque type
// parameters, are reported there; what our rules reject for the types of an
// instance (and TypeScript accepts) is reported where the instance is first
// used.
interface Dog { name: string }
const rex: Dog = { name: "Rex" }
class Show<T> {
  constructor(public x: T) {}
  text() { return "v: " + this.x }
}
console.log(new Show(5).text())
// expect-error: cannot use 'Show' with T = Dog: an object cannot be converted to a string
const shown = new Show(rex)
function show<T>(x: T) { return new Show(x).text() }
show(1)
// expect-error: cannot call 'show' with T = Dog: an object cannot be converted to a string
show(rex)
class Bad<T> {
  constructor(public x: T) {}
  // expect-error: operator '*' cannot be applied to types 'T' and 'int'
  twice() { return this.x * 2 }
}
class Twice<T> {
  add(x: T) { return 1 }
  add(x: string) { return 2 }
}
const t1 = new Twice<int>()
// expect-error: cannot use 'Twice' with T = string: duplicate variant add(x: string)
const t2 = new Twice<string>()

// Static members belong to the generic class, which every instance shares.
class Stat<T> {
  // expect-error: static field 'count' cannot use the type parameters of 'Stat': every instance of the class shares it
  static count: T
  // expect-error: cannot find type 'T'
  static make(x: T) { return 1 }
}

// Type arguments.
class Num<T extends number> { constructor(public n: T) {} }
// expect-error: type 'string' does not satisfy the constraint 'number' of 'T' in 'new Num'
const n1 = new Num("a")
// expect-error: type 'string' does not satisfy the constraint 'number' of 'T' in 'Num'
const n2: Num<string> = new Num(1)
class Z<T> { constructor(x: T) {} }
// expect-error: expected 1 argument, but got 0
const z = new Z()
class Empty<T> { items: T[] = [] }
// expect-error: cannot infer type parameter 'T' of 'new Empty': nothing in the call says what it is; give it, e.g. 'new Empty<int>(...)'
const e = new Empty()
// expect-error: 'Show' needs 1 type argument, e.g. 'Show<int>'
const y: Show = new Show(1)
// expect-error: 'Show' takes 1 type argument, but got 2
const y2: Show<int, int> = new Show(1)
class Plain {}
// expect-error: class 'Plain' is not generic; remove the type arguments
const p: Plain<int> = new Plain()
// expect-error: class 'Plain' is not generic; remove the type arguments
const p2 = new Plain<int>()
// expect-error: generic class 'Node' uses itself with type arguments that keep growing, so each instance of it needs another, without end
class Node<T> { next?: Node<T[]> }

// Generic methods.
class Base<T> { m(x: T) { return x } }
class Over<T> extends Base<T> {
  // expect-error: a generic method cannot be an override: each call runs a copy of it made for its types
  override f<U>(x: U) { return x }
}
abstract class Shape<T> {
  // expect-error: abstract method 'area' of generic class 'Shape' needs a return type, e.g. '): T'
  abstract area()
}
// expect-error: a method of an object literal cannot be generic; declare a class, whose methods can
const literal = { m<T>(x: T) { return 1 } }
class Box<T> {
  constructor(public v: T) {}
  get<U>(f: (x: T) => U) { return f(this.v) }
  size() { return 1 }
}
// expect-error: 'get' is a generic method, so it can only be called: each call runs a copy of it made for its types
const g = new Box(1).get
const box = new Box(1)
// expect-error: 'get' takes 1 type argument, but got 2
box.get<int, int>((x: int) => x)
// expect-error: 'size' is not a generic method; remove the type arguments
box.size<int>()

// instanceof of a generic class tells its instances apart by the type.
class Free { constructor(public x: int) {} }
const free: Free = new Free(1)
// expect-error: a 'Free' is never a 'Base', so this is always false
if (free instanceof Base) console.log("never")
class Animal { constructor(public name: string) {} }
class Pet<T> extends Animal { constructor(public toy: T) { super("pet") } }
const someAnimal: Animal = new Pet(1)
// expect-error: 'instanceof Pet' cannot tell which 'Pet' a 'Animal' would be
if (someAnimal instanceof Pet) console.log("pet")
