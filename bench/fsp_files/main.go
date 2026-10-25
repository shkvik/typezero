package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

// The same with a goroutine for each file, os.ReadFile and a WaitGroup.
func main() {
	const count = 10000
	dir := filepath.Join(os.TempDir(), "tz-bench-fsp-files")
	if _, err := os.Stat(dir); err != nil {
		making := fmt.Sprintf("%s-%d-%d", dir, len(os.Args), time.Now().UnixMilli())
		if err := os.Mkdir(making, 0o777); err != nil {
			panic(err)
		}
		for i := 0; i < count; i++ {
			text := fmt.Sprintf("file %d: %s\n", i, strings.Repeat("x", i%100))
			if err := os.WriteFile(filepath.Join(making, fmt.Sprintf("f%d.txt", i)), []byte(text), 0o666); err != nil {
				panic(err)
			}
		}
		if err := os.Rename(making, dir); err != nil {
			panic(err)
		}
	}
	texts := make([]string, count)
	var wg sync.WaitGroup
	for i := 0; i < count; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			data, err := os.ReadFile(filepath.Join(dir, fmt.Sprintf("f%d.txt", i)))
			if err != nil {
				panic(err)
			}
			texts[i] = string(data)
		}(i)
	}
	wg.Wait()
	total := 0
	for _, text := range texts {
		total += len(text)
	}
	fmt.Println(len(texts), total)
}
