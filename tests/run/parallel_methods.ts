// same-as-node
// A method changes the object it is called on. Tasks that run it on two
// objects made apart run in parallel; tasks whose objects may be one object
// (an array may hold it twice) keep node's order (docs/async.md, section 4).
class Account {
  name: string
  balance = 0

  constructor(name: string) {
    this.name = name
  }

  async deposit(amounts: int[]) {
    for (const amount of amounts) {
      await Promise.resolve()
      this.balance += amount
    }
  }
}

const alice = new Account("alice")
const bob = new Account("bob")
await Promise.all([alice.deposit([1, 2]), bob.deposit([3])])
console.log(alice.balance, bob.balance)

const accounts = [alice, bob, alice]
await Promise.all(accounts.map((account) => account.deposit([10])))
console.log(alice.balance, bob.balance)
