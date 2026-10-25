// same-as-node --experimental-transform-types
// Inheritance: `extends`, `super(...)` and `super.m()`, overriding, calls
// through the class up the chain (the method table of the object's class),
// instanceof with classes down the chain, protected members, inherited
// static members, and the order in which fields get their values.
const log: string[] = []

class Animal {
  static created = 0
  static create(name: string) {
    return new Animal(name)
  }
  legs = (log.push("Animal.legs"), 4)
  protected sound = "..."
  constructor(public name: string) {
    log.push(`Animal(${name})`)
    Animal.created++
  }
  speak() {
    return `${this.name} says ${this.sound}`
  }
  describe() {
    return `${this.name}, ${this.legs} legs: ${this.speak()}`
  }
}

class Dog extends Animal {
  tricks: string[] = (log.push("Dog.tricks"), [])
  constructor(name: string, public breed: string) {
    log.push("Dog before super")
    super(name)
    log.push("Dog after super")
    this.sound = "woof"
  }
  speak() {
    return `${super.speak()}!`
  }
  learn(trick: string) {
    this.tricks.push(trick)
    return this
  }
}

class Puppy extends Dog {
  speak() {
    return `${this.name} yips`
  }
}

class Bird extends Animal {
  legs = 2
  constructor(name: string) {
    super(name)
    this.sound = "tweet"
  }
}

const rex = new Dog("Rex", "lab")
console.log(log.join("; "))
rex.learn("sit").learn("roll")
console.log(rex)

const zoo: Animal[] = [rex, new Puppy("Bit", "pug"), new Bird("Kesha"), Animal.create("Thing")]
for (const a of zoo) console.log(a.describe())
console.log(zoo)
console.log(Animal.created, Dog.created, Puppy.create("x").name)

// instanceof: a class and the classes down its chain.
for (const a of zoo)
  console.log(a.name, a instanceof Animal, a instanceof Dog, a instanceof Puppy, a instanceof Bird)

// It narrows a variable to the class tested.
function tricks(a: Animal | undefined) {
  if (a instanceof Dog) return `${a.breed}: ${a.tricks.join(", ")}`
  return a == undefined ? "nobody" : "no tricks"
}
console.log(tricks(rex), tricks(zoo[2]), tricks(undefined))

// A Dog and a Bird are both Animals.
const pick = (dog: boolean) => (dog ? rex : new Bird("Tweety"))
console.log(pick(true).name, pick(false).describe())

// A method overridden down the chain, called from a method up the chain.
class Counter {
  count = 0
  step() {
    return 1
  }
  tick() {
    this.count += this.step()
    return this
  }
}
class Double extends Counter {
  step() {
    return 2
  }
}
const counters: Counter[] = [new Counter(), new Double()]
for (const c of counters) console.log(c.tick().tick().count)

// A class without a constructor takes the one up the chain.
class Base {
  constructor(public id: int, public label: string) {}
}
class Plain extends Base {
  extra = "e"
}
console.log(new Plain(7, "seven"))
