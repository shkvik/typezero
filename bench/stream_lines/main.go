package main

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// The same as a pipeline of goroutines: the file in chunks of 64 KiB, one
// that cuts them into lines, one that keeps those with "7" in upper case,
// and one that counts them, each sending to the next over a channel of 16,
// as the highWaterMark of an object-mode stream holds 16.
func main() {
	path := filepath.Join(os.TempDir(), "tz-bench-stream-lines.txt")
	if _, err := os.Stat(path); err != nil {
		var b strings.Builder
		seed := int64(1)
		for i := 0; i < 800000; i++ {
			seed = (seed*75 + 74) % 65537
			status := "ok"
			if seed%7 == 0 {
				status = "error"
			}
			fmt.Fprintf(&b, "line %d: value %d status %s %s\n", i, seed%100000, status,
				strings.Repeat("x", int(seed%9)))
		}
		making := fmt.Sprintf("%s-%d", path, time.Now().UnixMilli())
		if err := os.WriteFile(making, []byte(b.String()), 0o666); err != nil {
			panic(err)
		}
		if err := os.Rename(making, path); err != nil {
			panic(err)
		}
	}
	chunks := make(chan string, 1)
	lines := make(chan string, 16)
	sevens := make(chan string, 16)
	go func() {
		f, err := os.Open(path)
		if err != nil {
			panic(err)
		}
		defer f.Close()
		buf := make([]byte, 64*1024)
		for {
			n, err := f.Read(buf)
			if n > 0 {
				chunks <- string(buf[:n])
			}
			if err == io.EOF {
				break
			}
			if err != nil {
				panic(err)
			}
		}
		close(chunks)
	}()
	go func() {
		rest := ""
		for chunk := range chunks {
			parts := strings.Split(rest+chunk, "\n")
			rest = parts[len(parts)-1]
			for _, line := range parts[:len(parts)-1] {
				lines <- line
			}
		}
		if rest != "" {
			lines <- rest
		}
		close(lines)
	}()
	go func() {
		for line := range lines {
			if strings.Contains(line, "7") {
				sevens <- strings.ToUpper(line)
			}
		}
		close(sevens)
	}()
	count, total := 0, 0
	for line := range sevens {
		count++
		total += len(line)
	}
	fmt.Println(count, total)
}
