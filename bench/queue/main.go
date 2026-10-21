package main

import "fmt"

func main() {
	q := []int32{}
	for i := 0; i < 200000; i++ {
		q = append(q, int32(i))
	}
	var s int64
	for len(q) > 0 {
		s += int64(q[0])
		q = q[1:]
	}
	fmt.Println(s)
}
