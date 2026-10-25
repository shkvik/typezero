// A class decorator that returns a class that extends the class it got
// replaces it (docs/decorators.md, section 5): from where the declaration
// runs, the name means that class, and so does the type every annotation
// gives, those of the signatures written before too. Its members are seen
// and checked. `ConstructorArgs` are the parameters of the constructor of
// the class extended.

const created: object[] = []

function Registered<T extends Constructor>(cls: T) {
  return class extends cls {
    constructor(...args: ConstructorArgs) {
      super(...args) // the parameters of the class as declared
      created.push(this)
    }
  }
}

let nextId = 1
function WithId<T extends Constructor>(cls: T) {
  return class extends cls {
    id = nextId++
    describe() {
      return `#${this.id}`
    }
  }
}

function total(o: Order) {
  return `${o.describe()} of ${o.total}`
}

@WithId
class Order {
  constructor(public total: number) {}
  describe() {
    return "an order"
  }
  // In its own body the name means the class it became, as TypeScript has it.
  copy(): Order {
    return new Order(this.total)
  }
}

const order = new Order(100)
console.log(order.id, order, total(order), order.copy().id)

// Decorators apply from the bottom up: Registered gets the class WithId gave.
@Registered
@WithId
class Item {
  constructor(public name: string, public price: int) {}
}
const pen = new Item("pen", 3)
console.log(pen, pen.id, created.length, created[0] == pen)

// A class declared to extend a replaced class extends what the name means.
class BigOrder extends Order {
  big = true
}
const big = new BigOrder(5000)
const orders: Order[] = [order, big]
for (const o of orders) console.log(o.describe(), o.id, o instanceof Order)
console.log(big, Order.name == "", Item.name == "")

// A decorator that gives back the class it got leaves it as it is.
function Same<T extends Constructor>(cls: T) {
  return cls
}
@Same
class Kept {
  x = 1
}
console.log(new Kept(), Kept)
