// 2 GiB through SHA-256, in pieces of 1 MiB.
package main

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
)

func main() {
	piece := make([]byte, 1<<20)
	for i := range piece {
		piece[i] = byte(i)
	}
	h := sha256.New()
	for i := 0; i < 2048; i++ {
		h.Write(piece)
	}
	fmt.Println(hex.EncodeToString(h.Sum(nil)))
}
