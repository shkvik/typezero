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
	fmt.Println(len(strings.Join(words, ",")))
}
