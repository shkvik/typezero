// same-as-node --experimental-transform-types
// Generic classes in cycles, and generic functions over them.
class Link<T> {
  prev?: Link<T>
  next?: Link<T>
  constructor(public value: T) {}
}

class Ring<T> {
  first?: Link<T>
  size = 0
  add(value: T) {
    const link = new Link(value)
    const first = this.first
    if (!first) {
      link.prev = link
      link.next = link
      this.first = link
    } else {
      const last = first.prev ?? first
      link.prev = last
      link.next = first
      last.next = link
      first.prev = link
    }
    this.size++
    return this
  }
  values(): T[] {
    const out: T[] = []
    let at = this.first
    for (let i = 0; i < this.size && at; i++) {
      out.push(at.value)
      at = at.next
    }
    return out
  }
}

// A generic function that keeps what it is given.
function pairUp<T>(items: T[], pairs: T[][]) {
  for (let i = 0; i + 1 < items.length; i += 2) {
    const a = items[i], b = items[i + 1]
    if (a != undefined && b != undefined) pairs.push([a, b])
  }
}

let total = 0
for (let round = 0; round < 100; round++) {
  const numbers = new Ring<int>()
  for (let i = 0; i < 10; i++) numbers.add(i)
  const words = new Ring<string>().add("a").add("b").add("c")
  total += numbers.values().length + words.values().length
}
console.log(total)

const rings = [new Ring<string>().add("x"), new Ring<string>().add("y").add("z")]
const pairs: Ring<string>[][] = []
pairUp(rings, pairs)
console.log(pairs.length, pairs[0]?.[1]?.values().join(""))

// Rings of rings: a generic class whose values are its own kind.
const outer = new Ring<Ring<int>>()
for (let i = 0; i < 3; i++) outer.add(new Ring<int>().add(i).add(i * 2))
console.log(outer.values().map(r => r.values().join("+")).join(" "))
