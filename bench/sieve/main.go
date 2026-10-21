package main

import "fmt"

func count(n int) int {
	composite := make([]bool, n+1)
	found := 0
	for i := 2; i < len(composite); i++ {
		if composite[i] {
			continue
		}
		found++
		for j := i + i; j < len(composite); j += i {
			composite[j] = true
		}
	}
	return found
}

func main() { fmt.Println(count(20000000)) }
