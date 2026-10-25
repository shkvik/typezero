// Overloads with inheritance (docs/overloads.md, section 4): a subclass adds
// variants, overrides exactly one (`override` or not), and its constructors
// choose a variant of the one up the chain with `super(...)`. A call through
// the class up the chain chooses the variant by the type it knows, and the
// object's class gives the body. An interface may have several signatures
// of a method, and a class implements each with a variant. Node runs
// overloads wrongly, so this test is tz's own.
class Vec {
  constructor(public x: double, public y: double) {}
  constructor(p: Vec) {
    this.x = p.x
    this.y = p.y
  }
  add(v: Vec) {
    return new Vec(this.x + v.x, this.y + v.y)
  }
  add(k: double) {
    return new Vec(this.x + k, this.y + k)
  }
  show() {
    return `(${this.x}, ${this.y})`
  }
}

class Vec3 extends Vec {
  z = 0.0
  constructor(x: double, y: double, z: double) {
    super(x, y)
    this.z = z
  }
  constructor(v: Vec) {
    super(v)
  }
  override add(k: double) {
    return new Vec(this.x + 2 * k, this.y + 2 * k)
  }
  add(a: double, b: double) {
    return new Vec(this.x + a, this.y + b)
  }
  show() {
    return `(${this.x}, ${this.y}, ${this.z})`
  }
}

const v3 = new Vec3(1, 2, 3)
console.log(v3.add(1).show(), v3.add(new Vec(10, 10)).show(), v3.add(1, 2).show())
const asVec: Vec = v3
console.log(asVec.add(1).show(), asVec.add(asVec).show(), asVec.show())
console.log(new Vec3(new Vec(5, 6)).show())

// A class without a constructor of its own takes every variant up the chain.
class Point3 extends Vec3 {}
const p = new Point3(1, 1, 1)
const q = new Point3(new Vec(2, 2))
console.log(p.add(1).show(), q.show(), p instanceof Vec3)

// A method as a value is bound to its object, and the object's class gives
// the body.
const add: (k: double) => Vec = asVec.add
console.log(add(5).show())
const shows = [new Vec(1, 1), v3, p].map(v => v.show)
console.log(shows.map(f => f()).join(" "))

// An interface with several signatures of a method.
interface Addable {
  name: string
  add(k: double): string
  add(s: string): string
}
class Num implements Addable {
  constructor(public name: string, public v: double) {}
  add(k: double) {
    return `${this.v + k}`
  }
  add(s: string) {
    return `${this.v}${s}`
  }
}
class Text implements Addable {
  name = "text"
  add(k: double) {
    return `text+${k}`
  }
  add(s: string) {
    return `text+'${s}'`
  }
}
const addables: Addable[] = [new Num("n", 1.5), new Text()]
for (const a of addables) console.log(a.name, a.add(2), a.add("!"))
const first = addables[0]
if (first != undefined) {
  const f: (s: string) => string = first.add
  console.log(f("?"))
}
