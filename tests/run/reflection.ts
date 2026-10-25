// same-as-node --experimental-transform-types
// Reflection (docs/decorators.md, section 6): a class known only when the
// program runs makes objects with `new` and Reflect.construct; Reflect.get
// reads a field or a method by its name, and Reflect.apply calls what it
// gave. The exe describes the classes that reflection reaches.

class Account {
  balance = 0
  constructor(public owner: string, public limit?: int) {}
  deposit(amount: int) {
    this.balance += amount
    return this.balance
  }
  summary() {
    return `${this.owner}: ${this.balance}`
  }
}
class Savings extends Account {
  rate = 2
  summary() {
    return `${super.summary()} at ${this.rate}%`
  }
}

// A route table, as a router library keeps one: classes and method names.
type Route = { path: string; cls: Constructor; method: string; args: unknown[] }
const routes: Route[] = [
  { path: "/deposit", cls: Account, method: "deposit", args: [50] },
  { path: "/summary", cls: Savings, method: "summary", args: [] },
  { path: "/missing", cls: Account, method: "withdraw", args: [] },
]
function handle(path: string): unknown {
  const route = routes.find(r => r.path == path)
  if (route == undefined) return "404"
  const controller = new route.cls("web")
  const method = Reflect.get(controller, route.method)
  if (typeof method != "function") return "500"
  return Reflect.apply(method, controller, route.args)
}
console.log(handle("/deposit"), handle("/summary"), handle("/missing"), handle("/nope"))

// Fields by name, and what is not there.
const saver = Reflect.construct(Savings, ["ann", 10])
saver.deposit(5)
console.log(saver, Reflect.get(saver, "balance"), Reflect.get(saver, "owner"))
console.log(Reflect.get(saver, "limit"), Reflect.get(saver, "nothing"))
const summary = Reflect.get(saver, "summary")
console.log(summary, typeof summary, Reflect.apply(summary, saver, []))
// A method of a class applies to an object of a class that extends it.
const deposit = Reflect.get(new Account("x"), "deposit")
console.log(Reflect.apply(deposit, saver, [7]), saver.balance)

// A function in an unknown, and a class in a variable of `Constructor`.
const double: unknown = (n: int) => n * 2
console.log(Reflect.apply(double, undefined, [21]))
const kinds: Constructor<Account>[] = [Account, Savings]
for (const kind of kinds) {
  const made = Reflect.construct(kind, ["bob"])
  console.log(made.summary(), made instanceof Savings, kind.name)
}
