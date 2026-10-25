// Doubly linked lists: each node points at the one before it and the one
// after it, so a list is all cycles. 20 lists of 500 thousand nodes are
// built, walked both ways, cut in the middle and dropped; in tz each list is
// one group, freed at once when nothing holds it (in Go, the collector).
class ListNode {
  prev?: ListNode
  next?: ListNode
  constructor(public value: int) {}
}

class List {
  head?: ListNode
  tail?: ListNode
  push(value: int) {
    const node = new ListNode(value)
    const tail = this.tail
    if (tail) {
      tail.next = node
      node.prev = tail
    } else {
      this.head = node
    }
    this.tail = node
  }
}

let check = 0
for (let round = 0; round < 20; round++) {
  const list = new List()
  for (let i = 0; i < 500000; i++) list.push(i ^ round)
  let sum = 0
  for (let n = list.head; n; n = n.next) sum += n.value & 7
  for (let n = list.tail; n; n = n.prev) sum += n.value & 3
  // Unlink every tenth node.
  let i = 0
  for (let n = list.head; n; n = n.next) {
    const before = n.prev, after = n.next
    if (i++ % 10 == 0 && before && after) {
      before.next = after
      after.prev = before
    }
  }
  for (let n = list.head; n; n = n.next) sum += 1
  check = (check + sum) % 1000000007
}
console.log(check)
