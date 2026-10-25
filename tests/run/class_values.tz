// same-as-node --experimental-transform-types
// Classes as values (docs/decorators.md, section 5): `typeof C` is the class
// itself, known when compiling, in a variable, a parameter, a field; `new` of
// it and its static members are the class's. `Constructor<T>` is some class
// whose objects are T: arrays and maps of classes, `new` of one, instanceof.
// Class expressions make classes too.

class Animal {
  static made = 0
  constructor(public name: string) {
    Animal.made++
  }
  sound() {
    return "..."
  }
  static create(name: string) {
    return new Animal(name)
  }
}
class Dog extends Animal {
  sound() {
    return "woof"
  }
}
class Cat extends Animal {
  sound() {
    return "meow"
  }
}

// A class in a variable and as a parameter: the class itself.
const Kind = Dog
const rex = new Kind("rex")
console.log(rex, rex.sound(), Kind.name, Kind.made, Kind.create("x").name)
function adopt(kind: typeof Cat, name: string) {
  return new kind(name)
}
console.log(adopt(Cat, "tom"), typeof Kind, Kind == Dog)

// Classes known only when the program runs: an array of them, a map keyed by
// them, `new`, the name, instanceof.
const kinds: Constructor<Animal>[] = [Dog, Cat, Animal]
for (const k of kinds) {
  const a = new k("pet")
  console.log(k.name, a.sound(), a instanceof Dog, a instanceof k)
}
const counts = new Map<Constructor<Animal>, int>()
for (const k of [Dog, Cat, Dog])
  counts.set(k, (counts.get(k) ?? 0) + 1)
console.log(counts.get(Dog), counts.get(Cat), counts.get(Animal), counts.size)
function makeAll(list: Constructor<Animal>[], name: string): Animal[] {
  return list.map(k => new k(name))
}
console.log(makeAll(kinds, "z").map(a => a.sound()).join(" "))
const any: Constructor = Cat
console.log(any, any.name, [Dog, Cat])

// A test with a class that is a value narrows to its objects.
function find(pets: Animal[], kind: Constructor<Animal>) {
  for (const p of pets)
    if (p instanceof kind)
      return p.name
  return "none"
}
console.log(find([rex, new Cat("kit")], Cat), find([rex], Cat))

// Class expressions: named by the constant, by themselves, or not at all.
const Point = class {
  x = 1
  y = 2
  sum() {
    return this.x + this.y
  }
}
const p = new Point()
console.log(p, p.sum(), Point.name, Point)
const Named = class Inner {
  static count = 7
  hello() {
    return "hi"
  }
}
console.log(new Named().hello(), Named.name, Named.count, Named)
const Puppy = class extends Dog {
  sound() {
    return `small ${super.sound()}`
  }
}
const pup = new Puppy("bit")
console.log(pup, pup.sound(), pup instanceof Dog, Puppy)
const pets: Animal[] = [rex, pup, new Cat("c")]
console.log(pets.map(a => a.sound()))
console.log(Animal.made)
