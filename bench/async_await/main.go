package main

import (
	"fmt"
	"runtime"
)

// The same as close as Go gets to awaiting: a request to a goroutine and
// its reply over unbuffered channels for each call, and runtime.Gosched()
// for each plain value.
func main() {
	runtime.GOMAXPROCS(1)
	requests := make(chan int32)
	replies := make(chan int32)
	go func() {
		for i := range requests {
			replies <- i & 7
		}
	}()
	var sum int32
	for i := int32(0); i < 1000000; i++ {
		requests <- i
		sum += <-replies
	}
	for i := int32(0); i < 1000000; i++ {
		runtime.Gosched()
		sum += i & 3
	}
	fmt.Println(sum)
}
