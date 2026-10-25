// Inheritance: what a class that extends another may and may not do. Step 4
// of docs/objects.md is strict about writing through a wider type: a field
// keeps its type and an array of Dog is not an array of Animal.
class Animal {
  constructor(public name: string) {}
  speak() {
    return "..."
  }
  private secret = 1
  protected level = 2
  static count = 0
}

// `super(...)` comes before `this`, once, as a statement of the constructor.
class Dog extends Animal {
  breed = "lab"
  constructor(name: string) {
    // expect-error: 'this' cannot be used before 'super(...)': the object exists once the constructor of 'Animal' has run
    console.log(this.breed)
    super(name)
  }
}
class Cat extends Animal {
  constructor(name: string) {
    // expect-error: 'super(...)' must be a statement of its own in the body of the constructor
    if (name == "") super("anon")
    // expect-error: 'super(...)' must be a statement of its own in the body of the constructor
    else super(name)
  }
}
class Bird extends Animal {
  // expect-error: the constructor of 'Bird' must call 'super(...)': 'Bird' extends 'Animal', whose constructor sets the fields it declares
  constructor() {
    console.log("no super")
  }
}
class Bat extends Animal {
  constructor(name: string) {
    // expect-error: the constructor returns before 'super(...)', which makes the object
    if (name == "") return
    super(name)
  }
}
class Fish extends Animal {
  constructor() {
    super("fish")
    // expect-error: 'super(...)' is called a second time; the constructor of 'Animal' runs once
    super("again")
  }
  swim() {
    // expect-error: 'secret' is private in class 'Animal'
    return this.secret + this.level
  }
  // expect-error: 'swim2' is marked 'override', but 'Animal' has no method 'swim2'
  override swim2() {}
  // expect-error: 'speak' is marked 'override', but no method of 'Animal' has its parameters: there is speak(); an override takes the same parameters
  override speak(loud: boolean) {
    return "blub"
  }
}
// expect-error: 'level' is protected in class 'Animal'; only 'Animal' and the classes that extend it can use it
console.log(new Fish().level)
// expect-error: 'super(...)' can be called only in the constructor of a class that extends another
function notAConstructor() { super() }

// An override takes the parameters and returns the type of the method.
class Snake extends Animal {
  // expect-error: type 'int' is not assignable to type 'string'
  speak() { return 5 }
}
class Worm extends Animal {
  // expect-error: 'speak' returns 'int' in 'Worm', and 'string' in 'Animal', which it overrides
  speak(): int { return 0 }
}
class Mole extends Animal {
  // expect-error: 'speak' is public in 'Animal', so its override cannot be private
  private speak() { return "" }
}

// A field may narrow down the chain only to objects of a type down the chain
// of its own, which lie in memory the same way (and become read-only; code
// that writes through Kennel is checked in tests/errors/widen_fields.ts).
class Kennel {
  pet: Animal = new Animal("a")
  readonly size: int = 1
  open = true
}
class DogKennel extends Kennel {
  pet: Dog = new Dog("d")
  readonly open = false
  // expect-error: field 'size' of 'DogKennel' is 'double', and 'Kennel' declares it as 'int'; a class can narrow a field of the class it extends only to objects of a type down the chain of its type
  readonly size: double = 2
}
class Secretive extends Animal {
  // expect-error: 'secret' is private in class 'Animal', so a class that extends it cannot declare it again
  secret = 2
  // expect-error: 'speak' is a method of 'Animal', so a class that extends it cannot have a field of that name
  speak = 5
  // expect-error: 'name' is a field of 'Animal' already; declared again without a value, JavaScript would set it to undefined after the constructor of 'Animal' set it
  name: string
}
class Parent {
  label = "p"
  onClick = () => 1
}
class Child extends Parent {
  // expect-error: 'label' is a field of 'Parent', so a class that extends it cannot have a method of that name
  label() {}
  // expect-error: 'onClick' is an arrow in a field of 'Parent', which each object holds itself in JavaScript, so a method of a class that extends it would never run
  onClick() { return 2 }
}
// expect-error: 'override' says that 'x' is a field of the class that 'Lone' extends, and it extends none
class Lone { override x = 1 }

// An array of Dog is an array of Animal while no code puts another Animal
// into it (tests/errors/widen_arrays.ts).
const dogs: Dog[] = []
const animals: Animal[] = dogs
const copied: Animal[] = [...dogs]

// instanceof and the declared chain.
const someone: Animal = new Dog("x")
// expect-error: a 'Dog' is never a 'Cat': classes are told apart by their names, and neither extends the other, so this is always false
const never = new Dog("y") instanceof Cat
// expect-error: type 'Animal' is not assignable to type 'Dog'
const back: Dog = someone

// What `extends` names.
interface Named { name: string }
// expect-error: 'Named' is not a class, and a class can extend only a class; to say that 'Wrong' has what an interface lists, write 'implements Named'
class Wrong extends Named {}
// expect-error: 'ReferenceError' is not built in; extend Error, or one of its kinds that are: TypeError, RangeError, SyntaxError
class MyError extends ReferenceError {}
// expect-error: cannot find class 'Missing'
class Orphan extends Missing {}
class Loop1 extends Loop2 {}
// expect-error: class 'Loop2' cannot extend itself through 'Loop1'
class Loop2 extends Loop1 {}

// A constructor that calls a method a class down the chain overrides.
class Widget {
  size: int
  constructor() {
    this.size = 1
    // expect-error: the constructor of 'Widget' calls 'render', which 'Button' overrides: it would run before the fields of 'Button' are set; call it once the object is made
    this.render()
    // expect-error: the constructor of 'Widget' calls 'layout' (through the methods it calls), which 'Button' overrides
    this.refresh()
  }
  render() {}
  refresh() {
    this.layout()
  }
  layout() {}
}
class Button extends Widget {
  label = "ok"
  render() {
    console.log(this.label.length)
  }
  layout() {}
}

// super.x reads nothing: in JavaScript it reads the prototype.
class Reader extends Animal {
  read() {
    // expect-error: 'super.name' can only be called, 'super.name(...)'; to read the field or the method, use 'this.name'
    return super.name
  }
}

// Abstract classes and methods.
abstract class Figure {
  abstract area(): double
  abstract name(): string
}
// expect-error: class 'Blob' does not implement the abstract method area() of 'Figure'; implement it, or make 'Blob' abstract too
class Blob extends Figure {
  name() {
    return "blob"
  }
  describe() {
    // expect-error: 'area' is abstract in 'Figure': it has no body to call
    return super.area()
  }
}
// expect-error: class 'Figure' is abstract and cannot be created: create an object of a class that extends it
const figure = new Figure()
class Solid {
  // expect-error: abstract method 'volume' needs an abstract class: 'Solid' can be created with 'new', and the method would have no body; write 'abstract class Solid'
  abstract volume(): double
}
