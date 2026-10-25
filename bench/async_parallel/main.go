package main

import (
	"fmt"
	"sync"
)

// The same with a goroutine for each task and a WaitGroup for the counts.
func isPrime(n int32) bool {
	if n < 2 {
		return false
	}
	for d := int32(2); d*d <= n; d++ {
		if n%d == 0 {
			return false
		}
	}
	return true
}

func count(from, to int32) int32 {
	var found int32
	for n := from; n < to; n++ {
		if isPrime(n) {
			found++
		}
	}
	return found
}

func main() {
	counts := make([]int32, 64)
	var wg sync.WaitGroup
	for i := int32(0); i < 64; i++ {
		wg.Add(1)
		go func(i int32) {
			defer wg.Done()
			counts[i] = count(i*100000, (i+1)*100000)
		}(i)
	}
	wg.Wait()
	var total int32
	for _, c := range counts {
		total += c
	}
	fmt.Println(total)
}
