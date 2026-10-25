package main

import "fmt"

// The same as decimal, the way it is done by hand: int64 counting cents,
// which overflows silently past 92 233 720 368 547 758.07.
func main() {
	prices := []int64{}
	for i := 0; i < 1000000; i++ {
		prices = append(prices, int64(i%100000)+99)
	}
	var total int64
	for r := 0; r < 10; r++ {
		qty := int64(1)
		for _, p := range prices {
			total += p * qty
			qty = qty%5 + 1
		}
	}
	avg := total / 10000000
	if total%10000000 >= 5000000 {
		avg++
	}
	fmt.Println(cents(total), cents(avg))
}

// "1234.56" from 123456 cents.
func cents(v int64) string {
	s := fmt.Sprint(v)
	for len(s) < 3 {
		s = "0" + s
	}
	whole, frac := s[:len(s)-2], s[len(s)-2:]
	if frac == "00" {
		return whole
	}
	if frac[1] == '0' {
		return whole + "." + frac[:1]
	}
	return whole + "." + frac
}
