// 2 GiB through SHA-256: 128 pieces of 16 MiB, a goroutine for each, and the digest of their
// digests.
package main

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"sync"
)

func main() {
	piece := make([]byte, 16<<20)
	for i := range piece {
		piece[i] = byte(i)
	}
	pieces := make([][]byte, 128)
	for i := range pieces {
		p := make([]byte, len(piece))
		copy(p, piece)
		p[0] = byte(i)
		pieces[i] = p
	}
	digests := make([][32]byte, len(pieces))
	var wg sync.WaitGroup
	for i, p := range pieces {
		wg.Add(1)
		go func(i int, p []byte) {
			defer wg.Done()
			digests[i] = sha256.Sum256(p)
		}(i, p)
	}
	wg.Wait()
	all := make([]byte, 0, 32*len(digests))
	for _, d := range digests {
		all = append(all, d[:]...)
	}
	sum := sha256.Sum256(all)
	fmt.Println(hex.EncodeToString(sum[:]))
}
