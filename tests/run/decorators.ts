// Decorators of classes, fields, methods, parameters and static members, in
// the order TypeScript's experimentalDecorators applies them: the members of
// the instances, then the static ones, then the class with the parameters of
// its constructor; each declaration's decorators are evaluated top to bottom
// and applied bottom to top, its parameters' first.

function Sealed(cls: object) {
  console.log("class", cls)
}

function Column(cls: object, name: string) {
  console.log("field", name, "of", cls)
}

function Note(text: string) {
  console.log("evaluate", text)
  return (cls: object, name: string) => console.log("apply", text, "to", name)
}

function Inject(token: string) {
  console.log("evaluate inject", token)
  return (cls: object, method: string | undefined, index: int) =>
    console.log("parameter", index, "of", method ?? "the constructor", "gets", token)
}

// A decorator of the class that takes only what it needs.
function Named(prefix: string) {
  return (cls: object) => console.log(prefix, cls)
}

class Rules {
  static required(cls: object, name: string) {
    console.log("required", name)
  }
}

@Named("named") @Sealed
class User {
  @Column name: string
  @Rules.required @Column email = ""
  @Column static count = 0

  constructor(@Inject("NAME") name: string, @Inject("AGE") public age: int) {
    this.name = name
    User.count++
  }

  @Note("first") @Note("second")
  greet(@Inject("WHO") who: string, @Inject("HOW") how: string) {
    return `${this.name} greets ${who} ${how}`
  }

  @Note("static")
  static create() {
    return new User("Ann", 30)
  }
}

console.log("then the code after the class")
const ann = User.create()
console.log(ann.greet("Bob", "warmly"), User.count)

// A class is a value: what a decorator gets.
const cls = User
console.log(cls, cls.name, User.name, cls == User)
class Admin extends User {}
console.log(Admin, Admin.name)
const classes: object[] = [User, Admin, Rules]
console.log(classes)
console.log(typeof User, String, Number)

// Decorators of a class in another position, `export` and `abstract`.
@Sealed
export abstract class Shape {
  @Note("area") abstractArea() {
    return 0
  }
}
console.log("done")
