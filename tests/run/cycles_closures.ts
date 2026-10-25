// Cycles through functions: methods taken as values (bound to their
// objects in tz, as docs/objects.md section 5 says), closures that capture
// variables they and others change (boxes), and literals whose methods
// capture what holds the literal.
class Button {
  onClick?: () => void
  clicks = 0
  click() {
    this.clicks++
    this.onClick?.()
  }
}

class Dialog {
  button = new Button()
  closed = 0
  constructor(public title: string) {
    this.button.onClick = this.close
  }
  close() {
    this.closed++
  }
}

let closed = 0
for (let i = 0; i < 100; i++) {
  const d = new Dialog(`d${i}`)
  d.button.click()
  d.button.click()
  closed += d.closed
}
console.log(closed)

// A closure that calls itself through a variable it captures, which is
// assigned after it is made: the box of the variable holds the closure.
function countdown(n: int): int[] {
  const out: int[] = []
  let step: (k: int) => void = k => {}
  step = k => {
    out.push(k)
    if (k > 0) step(k - 1)
  }
  step(n)
  return out
}
let steps = 0
for (let i = 0; i < 50; i++) steps += countdown(5).length
console.log(steps)

// A literal whose method captures the variable that holds the literal.
interface Counter {
  n: int
  bump(): int
}
function makeCounter(): Counter {
  let self: Counter | undefined
  const c: Counter = {
    n: 0,
    bump() {
      if (self) self.n++
      return self?.n ?? -1
    },
  }
  self = c
  return c
}
const counters: Counter[] = []
for (let i = 0; i < 10; i++) {
  const c = makeCounter()
  c.bump()
  counters.push(c)
}
console.log(counters.map(c => c.bump()).join(","))
counters.length = 0

// Objects in cycles seen through an interface, a union and unknown.
interface Named {
  name: string
  friend?: Named
}
class Cat implements Named {
  friend?: Named
  constructor(public name: string) {}
}
class Dog implements Named {
  friend?: Named
  data: unknown = undefined
  constructor(public name: string) {}
}
type Pet = Cat | Dog
const pets: Pet[] = []
for (let i = 0; i < 20; i++) {
  const cat = new Cat(`c${i}`)
  const dog = new Dog(`d${i}`)
  cat.friend = dog
  dog.friend = cat
  dog.data = dog
  pets.push(i % 2 == 0 ? cat : dog)
}
console.log(pets.map(p => p.friend?.friend?.name).join(" ").length)
pets.length = 0
