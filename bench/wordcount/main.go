package main

import (
	"fmt"
	"sort"
	"strings"
)

// The text: 20 MB of words made of syllables from a fixed sequence (the same
// in main.ts), 3.5 million of them, some 160 thousand different.
var seed int32 = 42

func next(n int32) int32 {
	seed = (seed*1103515245 + 12345) & 0x7fffffff
	return int32(uint32(seed)>>16) % n
}

func input() string {
	syllables := []string{"ka", "ri", "to", "men", "sa", "lo", "pe", "dan", "vi", "ru", "ko", "shi",
		"nor", "el", "ta", "bu", "gar", "mi", "ze", "on"}
	parts := []string{}
	size := 0
	for size < 20000000 {
		w := syllables[next(20)]
		var extra int32
		if next(8) < 5 {
			extra = next(2)
		} else {
			extra = 1 + next(3)
		}
		for i := int32(0); i < extra; i++ {
			w += syllables[next(20)]
		}
		parts = append(parts, w)
		size += len(w) + 1
	}
	return strings.Join(parts, " ")
}

// A dictionary benchmark: counts the words of a text with a map[string]int,
// then shows the most frequent ones.
func main() {
	text := input()
	counts := map[string]int{}
	for _, word := range strings.Split(text, " ") {
		counts[word] = counts[word] + 1
	}
	type entry struct {
		word  string
		count int
	}
	all := []entry{}
	for w, n := range counts {
		all = append(all, entry{w, n})
	}
	sort.Slice(all, func(i, j int) bool {
		if all[i].count != all[j].count {
			return all[i].count > all[j].count
		}
		return all[i].word < all[j].word
	})
	top := []string{}
	for _, e := range all[:5] {
		top = append(top, fmt.Sprintf("%s %d", e.word, e.count))
	}
	fmt.Println(len(counts), strings.Join(top, ", "))
}
