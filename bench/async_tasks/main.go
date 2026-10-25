package main

import (
	"fmt"
	"runtime"
	"sync"
)

// The same with a goroutine for each task, which yields ten times, and a
// WaitGroup for the results.
func worker(id, rounds int32) int32 {
	var acc int32
	for r := int32(0); r < rounds; r++ {
		runtime.Gosched()
		acc += (id * r) & 15
	}
	return acc
}

func main() {
	results := make([]int32, 100000)
	var wg sync.WaitGroup
	for i := int32(0); i < 100000; i++ {
		wg.Add(1)
		go func(i int32) {
			defer wg.Done()
			results[i] = worker(i, 10)
		}(i)
	}
	wg.Wait()
	var total int32
	for _, r := range results {
		total += r
	}
	fmt.Println(total)
}
