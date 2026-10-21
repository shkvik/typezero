package main

import (
	"fmt"
	"strconv"
	"strings"
)

func main() {
	words := []string{}
	for i := 0; i < 1000000; i++ {
		words = append(words, "w"+strconv.Itoa(i%1000))
	}
	n := 0
	for _, w := range words {
		if strings.HasSuffix(w, "7") {
			n++
		}
	}
	fmt.Println(n)
}
