package main

import (
	"fmt"
	"iter"
	"slices"
)

// The same with Go's iterators (range over func, Go 1.23) and helpers like
// those of JavaScript, which the standard library lacks.

func rangeSeq(from, to int32) iter.Seq[int32] {
	return func(yield func(int32) bool) {
		for i := from; i < to; i++ {
			if !yield(i) {
				return
			}
		}
	}
}

func naturals() iter.Seq[int32] {
	return func(yield func(int32) bool) {
		for n := int32(0); ; n++ {
			if !yield(n) {
				return
			}
		}
	}
}

func filter[T any](seq iter.Seq[T], keep func(T) bool) iter.Seq[T] {
	return func(yield func(T) bool) {
		for x := range seq {
			if keep(x) && !yield(x) {
				return
			}
		}
	}
}

func mapSeq[T, U any](seq iter.Seq[T], f func(T) U) iter.Seq[U] {
	return func(yield func(U) bool) {
		for x := range seq {
			if !yield(f(x)) {
				return
			}
		}
	}
}

func take[T any](seq iter.Seq[T], limit int) iter.Seq[T] {
	return func(yield func(T) bool) {
		if limit <= 0 {
			return
		}
		left := limit
		for x := range seq {
			if !yield(x) {
				return
			}
			left--
			if left == 0 {
				return
			}
		}
	}
}

func reduce[T, A any](seq iter.Seq[T], f func(A, T) A, init A) A {
	acc := init
	for x := range seq {
		acc = f(acc, x)
	}
	return acc
}

func main() {
	const n = 10000000

	var multiples int64
	for i := range rangeSeq(0, n) {
		if i%3 == 0 {
			multiples += int64(i)
		}
	}

	doubled := reduce(
		take(mapSeq(filter(naturals(), func(x int32) bool { return x%3 == 0 }),
			func(x int32) int32 { return x * 2 }), 3000000),
		func(acc int64, x int32) int64 { return acc + int64(x) }, int64(0))

	a := []int32{}
	for i := 0; i < 1000; i++ {
		a = append(a, int32(i))
	}
	var big int64
	for round := int32(0); round < 10000; round++ {
		big += reduce(
			mapSeq(filter(slices.Values(a), func(x int32) bool { return x > round%100 }),
				func(x int32) int32 { return x * 3 }),
			func(acc int64, x int32) int64 { return acc + int64(x) }, int64(0))
	}

	fmt.Println(multiples, doubled, big)
}
