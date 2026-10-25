package main

import (
	"fmt"
	"strconv"
)

// Numbers to strings and back: a million integers and three million doubles
// in templates, String() and toFixed(2), then parseInt(), Number() and
// parseFloat() of a million strings each.
func main() {
	size := 0
	ints := []string{}
	decimals := []string{}
	for i := 0; i < 1000000; i++ {
		n := strconv.Itoa(i * 79)
		price := strconv.FormatFloat(float64(i)*0.37, 'f', -1, 64)
		size += len(n) + len(price) + len(strconv.FormatFloat(float64(i)/3.0, 'f', -1, 64)) +
			len(strconv.FormatFloat(float64(i)*0.01, 'f', 2, 64))
		ints = append(ints, n)
		decimals = append(decimals, price)
	}
	var sum int64
	big := 0
	for _, s := range ints {
		v, _ := strconv.Atoi(s)
		sum += int64(v)
	}
	for _, s := range decimals {
		if v, _ := strconv.ParseFloat(s, 64); v > 1000 {
			big++
		}
	}
	for _, s := range decimals {
		if v, _ := strconv.ParseFloat(s, 64); v < 1000 {
			big++
		}
	}
	fmt.Println(size, sum, big)
}
