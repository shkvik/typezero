package main

import "fmt"

func fold[T any, A any](xs []T, init A, f func(A, T) A) A {
	acc := init
	for _, x := range xs {
		acc = f(acc, x)
	}
	return acc
}

func indexOf[T comparable](xs []T, x T) int32 {
	for i := 0; i < len(xs); i++ {
		if xs[i] == x {
			return int32(i)
		}
	}
	return -1
}

func main() {
	a := []int32{}
	for i := 0; i < 10000000; i++ {
		a = append(a, int32(i&1023))
	}
	var zero int64
	var total int64
	for r := 0; r < 20; r++ {
		total += fold(a, zero, func(s int64, x int32) int64 { return s + int64(x) })
	}
	fmt.Println(total, indexOf(a, 1023), indexOf(a, 5000))
}
