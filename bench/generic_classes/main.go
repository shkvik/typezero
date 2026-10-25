package main

import (
	"cmp"
	"fmt"
)

type Stack[T any] struct {
	items []T
}

func (s *Stack[T]) Push(x T) { s.items = append(s.items, x) }

func (s *Stack[T]) Pop() (T, bool) {
	var zero T
	if len(s.items) == 0 {
		return zero, false
	}
	x := s.items[len(s.items)-1]
	s.items = s.items[:len(s.items)-1]
	return x, true
}

func (s *Stack[T]) Size() int32 { return int32(len(s.items)) }

type MinHeap[T cmp.Ordered] struct {
	items []T
}

func (q *MinHeap[T]) Size() int32 { return int32(len(q.items)) }

func (q *MinHeap[T]) Push(x T) {
	q.items = append(q.items, x)
	h := q.items
	i := len(h) - 1
	for i > 0 && i < len(h) {
		p := (i - 1) >> 1
		if p < 0 || p >= len(h) {
			break
		}
		child, parent := h[i], h[p]
		if parent <= child {
			break
		}
		h[i] = parent
		h[p] = child
		i = p
	}
}

func (q *MinHeap[T]) Pop() (T, bool) {
	var zero T
	h := q.items
	if len(h) == 0 {
		return zero, false
	}
	last := h[len(h)-1]
	h = h[:len(h)-1]
	q.items = h
	if len(h) == 0 {
		return last, true
	}
	top := h[0]
	h[0] = last
	n := len(h)
	i := 0
	for i >= 0 && i < n {
		l := 2*i + 1
		if l < 0 || l >= n {
			break
		}
		m := l
		r := l + 1
		if r < n && h[r] < h[l] {
			m = r
		}
		if m >= n {
			break
		}
		at, child := h[i], h[m]
		if at <= child {
			break
		}
		h[i] = child
		h[m] = at
		i = m
	}
	return top, true
}

func main() {
	stack := &Stack[int32]{}
	for i := 0; i < 10000000; i++ {
		stack.Push(int32(i & 1023))
	}
	var total int64
	for stack.Size() > 0 {
		x, _ := stack.Pop()
		total += int64(x)
	}

	heap := &MinHeap[int32]{}
	var seed int32 = 12345
	for i := 0; i < 1000000; i++ {
		seed = (seed*1103515245 + 12345) & 0x7fffffff
		heap.Push(seed % 1000000)
	}
	var previous int32 = -1
	sorted := true
	var checksum int64
	k := 0
	for heap.Size() > 0 {
		x, _ := heap.Pop()
		if x < previous {
			sorted = false
		}
		previous = x
		checksum += int64(x * int32(k%7))
		k++
	}
	fmt.Println(total, sorted, checksum)
}
