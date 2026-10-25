// Every field gets a value before the object is used: until the
// constructor has set them all, `this` only sets fields and reads the ones
// set (docs/objects.md, section 6).
class User {
  name: string
  email: string
  constructor(name: string) {
    this.name = name
    // expect-error: 'this.log()' is called before every field is set: field 'email' is not set yet, and the method may read it
    this.log()
    // expect-error: field 'email' is read before it is set
    console.log(this.email)
    // expect-error: 'this' is used before every field is set: field 'email' is not set yet; until then 'this' can only set fields and read the ones set
    register(this)
    // expect-error: this function uses 'this', and it is created before every field is set; create it after the constructor sets them
    const early = () => this.name
    this.email = `${name}@example.com`
    // Every field is set now.
    this.log()
    register(this)
    const late = () => this.email
    console.log(late(), this.name)
  }
  log() {
    console.log(`${this.name} <${this.email}>`)
  }
}
function register(user: User) {
  console.log(user.name)
}

// Initializers run in order, before the constructor and its parameter
// properties.
class Order {
  // expect-error: field 'price' is read before it is set
  total = this.price * 2
  price = 10
  constructor(public id: int) {}
}
class Late {
  // expect-error: field 'id' is read before it is set
  label = `#${this.id}`
  constructor(public id: int) {}
}

// A field without a value is set on every path of every constructor.
class Branchy {
  x: int
  // expect-error: the constructor of 'Branchy' does not set field 'x' on every path; set it, give it a value where it is declared, or make it optional: 'x?: T'
  constructor(flag: boolean) {
    if (flag) this.x = 1
  }
}
class Returns {
  x: int
  constructor(flag: boolean) {
    if (flag) {
      this.x = 1
      return
    }
    // expect-error: the constructor of 'Returns' does not set field 'x' on every path
    return
  }
}
class Loop {
  x: int
  // expect-error: the constructor of 'Loop' does not set field 'x' on every path
  constructor(n: int) {
    for (let i = 0; i < n; i++) this.x = i
  }
}
class Counter {
  count: int
  constructor() {
    // expect-error: field 'count' is read before it is set
    this.count += 1
  }
}
class NoConstructor {
  // expect-error: field 'size' never gets a value: give it one here, e.g. 'size: int = ...', set it in a constructor, or make it optional: 'size?: int'
  size: int
  ok = 1
  maybe?: int
}
class Definite {
  // expect-error: 'data!' ("set later") is not supported: a field gets its value where it is declared or in the constructor; if it may have none, make it optional: 'data?: T'
  data!: string
}

// Optional fields hold undefined from the start, and fields with values
// are set before the constructor runs.
class Fine {
  limit?: int
  count = 0
  name: string
  constructor() {
    console.log(this.limit, this.count)
    this.name = "fine"
  }
}
