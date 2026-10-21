package main

import (
	"fmt"
	"strings"
)

// The same code as the TypeScript version: += on strings.
func main() {
	s := ""
	for i := 0; i < 200000; i++ {
		s += "ab"
	}
	t := ""
	for i := 0; i < 100000; i++ {
		t += "ab"
		if strings.HasSuffix(t, "q") {
			fmt.Println("never")
		}
	}
	fmt.Println(len(s), len(t))
}
