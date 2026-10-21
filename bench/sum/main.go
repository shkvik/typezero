package main

import "fmt"

func main() {
	a := []int32{}
	for i := 0; i < 10000000; i++ {
		a = append(a, int32(i&1023))
	}
	var total int64
	for r := 0; r < 20; r++ {
		var s int64
		for i := 0; i < len(a); i++ {
			s += int64(a[i])
		}
		total += s
	}
	fmt.Println(total)
}
