// same-as-node --experimental-transform-types
// Objects released through a type up their chain free what their own class
// holds, and a long list of objects of mixed classes frees without deep
// recursion, as a list of one class does. The test runs with leak checks.
class Node {
  next?: Node
  constructor(public value: int) {}
}
class Tagged extends Node {
  tags: string[] = []
  constructor(value: int, public label: string) {
    super(value)
    this.tags.push(`t${value % 10}`)
  }
}
let head: Node | undefined = undefined
for (let i = 0; i < 300000; i++) {
  const n = i % 3 == 0 ? new Node(i) : new Tagged(i, `n${i}`)
  n.next = head
  head = n
}
let sum: long = 0
let tagged = 0
for (let cur = head; cur != undefined; cur = cur.next) {
  sum += cur.value
  if (cur instanceof Tagged) tagged += cur.tags.length
}
console.log(sum, tagged)
head = undefined

interface Holder {
  items: string[]
}
class Box implements Holder {
  items = ["a", "b"]
  label = "box"
}
class BigBox extends Box {
  more = [["x"], ["y", "z"]]
}
const holders: Holder[] = [new Box(), new BigBox(), { items: ["c"] }]
console.log(holders.map(h => h.items.length).join(" "))
holders.pop()
holders[0] = new BigBox()
console.log(holders.length)
