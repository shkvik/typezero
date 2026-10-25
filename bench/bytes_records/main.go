package main

import (
	"encoding/binary"
	"fmt"
	"math"
	"strconv"
)

func main() {
	count := 4 * 1024 * 1024
	data := make([]byte, count*16)
	seed := int32(1)
	for off := 0; off+16 <= len(data); off += 16 {
		seed = seed*1103515245 + 12345
		binary.LittleEndian.PutUint32(data[off:], uint32(seed))
		binary.LittleEndian.PutUint64(data[off+4:], math.Float64bits(float64(seed&0xffff)/64))
		binary.BigEndian.PutUint16(data[off+12:], uint16(seed&0xffff))
		binary.BigEndian.PutUint16(data[off+14:], uint16((uint32(seed)>>16)&0xffff))
	}
	var ids int64
	values := 0.0
	var small int64
	for round := 0; round < 5; round++ {
		for off := 0; off+16 <= len(data); off += 16 {
			ids += int64(binary.LittleEndian.Uint32(data[off:]))
			values += math.Float64frombits(binary.LittleEndian.Uint64(data[off+4:]))
			small += int64(binary.BigEndian.Uint16(data[off+12:]) ^ binary.BigEndian.Uint16(data[off+14:]))
		}
	}
	buf := data[:] // Buffer.from(arrayBuffer) shares the memory
	var check int64
	total := 0.0
	for off := 0; off+16 <= len(buf); off += 16 {
		check += int64(binary.LittleEndian.Uint32(buf[off:]))
		total += math.Float64frombits(binary.LittleEndian.Uint64(buf[off+4:]))
	}
	fmt.Println(ids, strconv.FormatFloat(values, 'f', -1, 64), small, check,
		strconv.FormatFloat(total, 'f', -1, 64))
}
