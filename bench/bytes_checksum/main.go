package main

import "fmt"

func fill(data []byte) {
	for i := 0; i < len(data); i++ {
		data[i] = byte((int32(i)*7 + int32(uint32(i)>>9)) & 0xff)
	}
}

func xor(data []byte, key []byte) {
	if len(key) < 16 {
		return
	}
	for i := 0; i < len(data); i++ {
		data[i] ^= key[i&15]
	}
}

func sum(data []byte) int64 {
	var s int64
	for i := 0; i < len(data); i++ {
		s += int64(data[i])
	}
	return s
}

func fnv1a(data []byte) int64 {
	h := uint32(0x811c9dc5)
	for i := 0; i < len(data); i++ {
		h = (h ^ uint32(data[i])) * 0x01000193
	}
	return int64(h)
}

func main() {
	data := make([]byte, 100*1024*1024)
	key := []byte{0x13, 0x37, 0xc0, 0xde, 0xba, 0xad, 0xf0, 0x0d,
		0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef}
	fill(data)
	before := sum(data)
	xor(data, key)
	encrypted := fnv1a(data)
	xor(data, key)
	fmt.Println(before, encrypted, sum(data), fnv1a(data))
}
