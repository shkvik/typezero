package main

import "fmt"

func main() {
	a := []int32{}
	for i := 0; i < 10000000; i++ {
		a = append(a, int32(i&1023))
	}
	fmt.Println(len(a))
}
