package main

import (
	"fmt"
	"regexp"
	"strings"
)

// The text: 20 MB of words, numbers and dates from a fixed sequence (the
// same in main.ts).
var seed int32 = 42

func next(n int32) int32 {
	seed = (seed*1103515245 + 12345) & 0x7fffffff
	return int32(uint32(seed)>>16) % n
}

func input() string {
	words := []string{"running", "data", "error", "the", "warning", "loading", "value", "fatal",
		"string", "parse", "agggtaaa", "TTTACCCT", "thing", "king", "of", "compile", "ringing",
		"acgt", "x1", "warningX"}
	parts := []string{}
	size := 0
	for size < 20000000 {
		var w string
		if next(20) == 0 {
			year := 2000 + next(30)
			month := 1 + next(12)
			day := 1 + next(28)
			w = fmt.Sprintf("%d-%02d-%02d", year, month, day)
		} else {
			w = words[next(int32(len(words)))]
		}
		parts = append(parts, w)
		size += len(w) + 1
	}
	return strings.Join(parts, " ")
}

// A regex benchmark (docs/regex.md):
// walks the matches of `(\w+)ing (\w+)` with their groups (`matchAll`).
func main() {
	text := input()
	n := 0
	for _, m := range regexp.MustCompile(`(\w+)ing (\w+)`).FindAllStringSubmatch(text, -1) {
		n += len(m[2])
	}
	fmt.Println(n)
}
