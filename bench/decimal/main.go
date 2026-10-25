package main

import (
	"fmt"
	"math/big"
)

// Exact money in the standard library: big.Int counting cents, updated in
// place so that the loop allocates nothing.
func main() {
	prices := []*big.Int{}
	for i := 0; i < 1000000; i++ {
		prices = append(prices, big.NewInt(int64(i%100000)+99))
	}
	total := new(big.Int)
	t := new(big.Int)
	q := new(big.Int)
	for r := 0; r < 10; r++ {
		qty := int64(1)
		for _, p := range prices {
			t.Mul(p, q.SetInt64(qty))
			total.Add(total, t)
			qty = qty%5 + 1
		}
	}
	// The average, rounded half up to cents.
	avg := new(big.Int)
	rest := new(big.Int)
	avg.QuoRem(total, big.NewInt(10000000), rest)
	if rest.Cmp(big.NewInt(5000000)) >= 0 {
		avg.Add(avg, big.NewInt(1))
	}
	fmt.Println(cents(total), cents(avg))
}

// "1234.56" from 123456 cents.
func cents(v *big.Int) string {
	s := v.String()
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
