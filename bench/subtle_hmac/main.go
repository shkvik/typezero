// 200 000 HMAC-SHA256 signatures of messages of 1 KiB with one key, as a server checks the
// requests it gets: 1000 at a time, a goroutine for each; the XOR of the first bytes of each.
package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"fmt"
	"sync"
)

func main() {
	key := []byte("a secret of the server")
	message := make([]byte, 1024)
	for i := range message {
		message[i] = byte(i)
	}
	check := byte(0)
	for batch := 0; batch < 200; batch++ {
		signatures := make([][]byte, 1000)
		var wg sync.WaitGroup
		for i := 0; i < 1000; i++ {
			wg.Add(1)
			go func(i int) {
				defer wg.Done()
				m := make([]byte, len(message))
				copy(m, message)
				m[0] = byte(i)
				m[1] = byte(batch)
				mac := hmac.New(sha256.New, key)
				mac.Write(m)
				signatures[i] = mac.Sum(nil)
			}(i)
		}
		wg.Wait()
		for _, s := range signatures {
			check ^= s[0]
		}
	}
	fmt.Println(check)
}
