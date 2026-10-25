// Classes: names, access, methods, `this`, static members.
class Point {
  constructor(public x: double, public y: double) {}
  length() {
    return Math.hypot(this.x, this.y)
  }
}
class Size {
  constructor(public x: double, public y: double) {}
}

// Classes are told apart by their names; a literal is never an instance.
// expect-error: type '{ x: int; y: int }' is not assignable to type 'Point'; an object literal is never an instance of a class: create one with 'new Point(...)'
const p: Point = { x: 1, y: 2 }
// expect-error: type 'Size' is not assignable to type 'Point'; classes are told apart by their names, whatever their fields
const q: Point = new Size(1, 2)
// expect-error: an object of type '{ x: int }' is never a 'Point': only 'new Point(...)' creates one, so this is always false
const lit = { x: 1 } instanceof Point
// expect-error: a 'Size' is never a 'Point': classes are told apart by their names, and neither extends the other, so this is always false
const size = new Size(1, 2) instanceof Point
// expect-error: 'instanceof' tests an object, found 'int', which is never an instance of a class
const num = 5 instanceof Point
// expect-error: the right side of 'instanceof' must be a class
const notClass = new Point(1, 2) instanceof size
// expect-error: there are no prototypes in tz: the methods belong to the class itself
const proto = Point.prototype

// Members that are not there, and the wrong way to reach the static ones.
const pt = new Point(3, 4)
// expect-error: property 'z' does not exist on type 'Point'
console.log(pt.z)
// expect-error: property 'z' does not exist on type 'Point'
pt.z = 1
// expect-error: class 'Point' has no static member 'origin'
console.log(Point.origin)
// expect-error: 'length' is a member of the instances of 'Point', not of the class: create one with 'new Point(...)'
console.log(Point.length)
// A method cannot be replaced.
// expect-error: 'length' is a method of 'Point' and cannot be replaced: a method belongs to the class, not to the object; to change what it does, declare a field that holds a function: 'length: () => void = () => { ... }'
pt.length = () => 0

class Registry {
  static count = 0
  static readonly limit = 10
  static next() {
    return ++Registry.count
  }
  name = "r"
}
const reg = new Registry()
// expect-error: 'count' is a static member of 'Registry': use it through the class, 'Registry.count'
console.log(reg.count)
// expect-error: 'next' is a static method of 'Registry': call it through the class, 'Registry.next(...)'
reg.next()
// expect-error: cannot assign to 'Registry.limit' because it is a read-only static field
Registry.limit = 5
// expect-error: 'next' is a static method of 'Registry' and cannot be replaced
Registry.next = () => 0

// Private and protected members are for the class itself.
class Vault {
  #code = 1234
  private owner = "Ann"
  protected level = 2
  readonly opened: int = 0
  private constructor() {}
  static open() {
    const v = new Vault()
    console.log(v.#code + v.level)
    return v
  }
  check(other: Vault) {
    return other.#code == this.#code && other.owner == this.owner
  }
}
// expect-error: 'constructor' is private in class 'Vault'
const vault = new Vault()
const opened = Vault.open()
// expect-error: '#code' is private in class 'Vault'
console.log(opened.#code)
// expect-error: 'owner' is private in class 'Vault'
console.log(opened.owner)
// expect-error: 'level' is protected in class 'Vault'; only 'Vault' and the classes that extend it can use it
console.log(opened.level)
// expect-error: cannot assign to 'opened' because it is a read-only field of 'Vault'; only its constructor can set it
opened.opened = 1

// `this` exists in methods, and arrows see the method's.
// expect-error: 'this' is used outside of a method; it exists in the methods of classes and object literals
const self = this
class Box {
  static size = 1
  // expect-error: in a static field 'this' would be the class itself; use the class's name: 'Box.size'
  static double = this.size * 2
  value = 1
  static make() {
    // expect-error: in a static method 'this' would be the class itself; use the class's name: 'Box.size'
    return this.size
  }
  plain() {
    function inner() {
      // expect-error: a 'function' has no 'this' of its own; use an arrow function, which sees the 'this' of the method around it
      return this.value
    }
    // expect-error: cannot assign to 'this'
    this = new Box()
    // expect-error: 'super.value' can only be called, 'super.value(...)'; to read the field or the method, use 'this.value'
    return super.value
  }
  // expect-error: a constructor cannot return a value: 'new' gives the object
  constructor() { return 5 }
}

// An instance is not converted to a string by calling code.
class Money {
  constructor(public cents: int) {}
  toString() {
    return `${this.cents / 100}`
  }
}
const price = new Money(150)
// expect-error: an object is not converted to a string by calling its toString(): tz calls no code by itself; call it, e.g. '${price.toString()}'
console.log(`price: ${price}`)
// expect-error: an object is always truthy, so the condition is always true
if (price) console.log("yes")

// A class's constructor takes what it declares.
// expect-error: class 'Registry' has no constructor, so 'new Registry()' takes no arguments
const noCtor = new Registry(1)
// expect-error: expected 2 arguments, but got 1
const missing = new Point(1)
// expect-error: type 'string' is not assignable to type 'double'
const wrong = new Point("a", 2)

// Declarations.
class Dup {
  a = 1
  // expect-error: duplicate member 'a' in class 'Dup'
  a = 2
  // expect-error: duplicate member 'b' in class 'Dup': a field and a method cannot share a name
  b() {}
  b = 3
  // expect-error: field 'c' needs a type, e.g. 'c: int'
  c
  // expect-error: static field 'd' needs a value, e.g. 'static d: int = ...', or make it optional: 'static d?: int'
  static d: int
}
// expect-error: duplicate declaration of 'Point'
function Point() {}
// expect-error: 'Late.n' is used before class 'Late' is declared, which sets it
console.log(Late.n)
// expect-error: class 'Late' is used before its declaration, which JavaScript has not run yet here (a ReferenceError); move the class up
const early = new Late()
class Late {
  static n = 1
  // expect-error: 'Late.m' is read before it is set: the static fields are set in the order they are declared
  static k = Late.m
  static m = 2
}
{
  // expect-error: a class can be declared only at the top level of a file
  class InBlock {}
}
function local() {
  // expect-error: a class can be declared only at the top level of a file
  class InFunction {}
}
