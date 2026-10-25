// same-as-node --experimental-transform-types
// A doubly linked list: each node points at the one before it and the one
// after it, so the nodes form cycles in pairs.
class ListNode {
  prev?: ListNode
  next?: ListNode
  constructor(public value: int) {}
}

class List {
  head?: ListNode
  tail?: ListNode
  length = 0
  push(value: int) {
    const node = new ListNode(value)
    if (this.tail) {
      this.tail.next = node
      node.prev = this.tail
    } else {
      this.head = node
    }
    this.tail = node
    this.length++
    return node
  }
  unshift(value: int) {
    const node = new ListNode(value)
    if (this.head) {
      this.head.prev = node
      node.next = this.head
    } else {
      this.tail = node
    }
    this.head = node
    this.length++
  }
  remove(node: ListNode) {
    if (node.prev) node.prev.next = node.next
    else this.head = node.next
    if (node.next) node.next.prev = node.prev
    else this.tail = node.prev
    node.prev = undefined
    node.next = undefined
    this.length--
  }
  forward(): int[] {
    const out: int[] = []
    for (let n = this.head; n; n = n.next) out.push(n.value)
    return out
  }
  backward(): int[] {
    const out: int[] = []
    for (let n = this.tail; n; n = n.prev) out.push(n.value)
    return out
  }
}

const list = new List()
for (let i = 1; i <= 5; i++) list.push(i * 10)
list.unshift(0)
console.log(list.forward().join(" "), "|", list.backward().join(" "))
const third = list.head?.next?.next
if (third) list.remove(third)
console.log(list.forward().join(" "), list.length)

// Long lists, made and dropped: freeing takes no stack.
let sum = 0
for (let round = 0; round < 5; round++) {
  const big = new List()
  for (let i = 0; i < 100000; i++) big.push(i)
  sum += big.length
}
console.log(sum)

// A node removed from a list lives on its own; one held keeps its list.
const short = new List()
const kept = short.push(1)
short.push(2)
short.push(3)
short.remove(kept)
console.log(kept.value, kept.prev == undefined, short.forward().join(","))
let middle: ListNode | undefined = new List().push(7)
middle = undefined
console.log(middle == undefined)
