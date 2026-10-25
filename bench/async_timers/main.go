package main

import (
	"fmt"
	"sync"
	"time"
)

// The same with time.AfterFunc; a timer that Stop() did not stop in time
// counts as fired, so the total is the same.
func main() {
	var mu sync.Mutex
	fired, cleared := 0, 0
	var wg sync.WaitGroup
	timers := make([]*time.Timer, 0, 200000)
	for i := 0; i < 200000; i++ {
		wg.Add(1)
		timers = append(timers, time.AfterFunc(time.Duration(1+i%10)*time.Millisecond, func() {
			mu.Lock()
			fired++
			mu.Unlock()
			wg.Done()
		}))
	}
	for i := 0; i < len(timers); i += 4 {
		if timers[i].Stop() {
			cleared++
			wg.Done()
		}
	}
	wg.Wait()
	fmt.Println(fired + cleared)
}
