package main

import (
	"fmt"
	"strings"
)

// What a Go programmer writes instead: strings.Builder.
func main() {
	var s strings.Builder
	for i := 0; i < 200000; i++ {
		s.WriteString("ab")
	}
	var t strings.Builder
	for i := 0; i < 100000; i++ {
		t.WriteString("ab")
		if strings.HasSuffix(t.String(), "q") {
			fmt.Println("never")
		}
	}
	fmt.Println(s.Len(), t.Len())
}
