// 2 GiB encrypted with AES-256-GCM: 131072 messages of 16 KiB (a record of TLS), each with a
// nonce of its own; the SHA-256 of their tags is printed.
package main

import (
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"fmt"
)

func main() {
	key := bytes.Repeat([]byte{7}, 32)
	piece := make([]byte, 1<<14)
	for i := range piece {
		piece[i] = byte(i)
	}
	block, _ := aes.NewCipher(key)
	gcm, _ := cipher.NewGCM(block)
	nonce := make([]byte, 12)
	tags := sha256.New()
	out := make([]byte, 0, len(piece)+16)
	for i := 0; i < 131072; i++ {
		binary.BigEndian.PutUint32(nonce, uint32(i))
		out = gcm.Seal(out[:0], nonce, piece, nil)
		tags.Write(out[len(piece):])
	}
	fmt.Println(hex.EncodeToString(tags.Sum(nil)))
}
