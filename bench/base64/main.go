package main

import (
	"bytes"
	"encoding/base64"
	"encoding/hex"
	"fmt"
)

func main() {
	data := make([]byte, 30*1024*1024)
	x := int32(12345)
	for i := 0; i < len(data); i++ {
		x = x*1103515245 + 12345
		data[i] = byte(uint32(x) >> 24)
	}
	buf := append([]byte(nil), data...)
	length := 0
	same := true
	for round := 0; round < 5; round++ {
		text := base64.StdEncoding.EncodeToString(buf)
		length += len(text)
		back, _ := base64.StdEncoding.DecodeString(text)
		same = same && bytes.Equal(back, buf)
	}
	h := hex.EncodeToString(buf)
	back, _ := hex.DecodeString(h)
	fmt.Println(length, same, len(h), h[:16], bytes.Equal(back, buf))
}
