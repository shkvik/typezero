package main

import "fmt"

type ListNode struct {
	prev, next *ListNode
	value      int32
}

type List struct{ head, tail *ListNode }

func (l *List) push(value int32) {
	node := &ListNode{value: value}
	if l.tail != nil {
		l.tail.next = node
		node.prev = l.tail
	} else {
		l.head = node
	}
	l.tail = node
}

func main() {
	check := int32(0)
	for round := int32(0); round < 20; round++ {
		list := &List{}
		for i := int32(0); i < 500000; i++ {
			list.push(i ^ round)
		}
		sum := int32(0)
		for n := list.head; n != nil; n = n.next {
			sum += n.value & 7
		}
		for n := list.tail; n != nil; n = n.prev {
			sum += n.value & 3
		}
		i := int32(0)
		for n := list.head; n != nil; n = n.next {
			before, after := n.prev, n.next
			if i%10 == 0 && before != nil && after != nil {
				before.next = after
				after.prev = before
			}
			i++
		}
		for n := list.head; n != nil; n = n.next {
			sum += 1
		}
		check = (check + sum) % 1000000007
	}
	fmt.Println(check)
}
