// The load of the HTTP benchmark (bench/http.ps1), as bombardier makes it:
// `connections` connections kept alive, each sending a GET and reading the
// response, again and again, for `seconds`. Prints the requests a second
// and the mean latency.
//
//	go run ./load -url 127.0.0.1:8080 -c 128 -d 10s
package main

import (
	"bufio"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

func main() {
	addr := flag.String("url", "127.0.0.1:8080", "host:port of the server")
	connections := flag.Int("c", 128, "connections")
	duration := flag.Duration("d", 10*time.Second, "how long")
	flag.Parse()

	request := []byte("GET / HTTP/1.1\r\nHost: " + *addr + "\r\nUser-Agent: load\r\n\r\n")
	var done, failed, latency atomic.Int64
	deadline := time.Now().Add(*duration)
	var wg sync.WaitGroup
	for i := 0; i < *connections; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			conn, err := net.Dial("tcp", *addr)
			if err != nil {
				failed.Add(1)
				return
			}
			defer conn.Close()
			r := bufio.NewReaderSize(conn, 16384)
			for time.Now().Before(deadline) {
				start := time.Now()
				if _, err := conn.Write(request); err != nil {
					failed.Add(1)
					return
				}
				if !readResponse(r) {
					failed.Add(1)
					return
				}
				latency.Add(int64(time.Since(start)))
				done.Add(1)
			}
		}()
	}
	wg.Wait()
	n := done.Load()
	if n == 0 {
		fmt.Println("no responses")
		os.Exit(1)
	}
	fmt.Printf("%.0f req/s, %.2f ms, %d failed\n", float64(n)/duration.Seconds(),
		float64(latency.Load())/float64(n)/1e6, failed.Load())
}

// Reads one response, with a Content-Length or in chunks.
func readResponse(r *bufio.Reader) bool {
	length := 0
	chunked := false
	status := true
	for {
		line, err := r.ReadString('\n')
		if err != nil {
			return false
		}
		if status {
			if !strings.HasPrefix(line, "HTTP/1.1 200") {
				return false
			}
			status = false
			continue
		}
		if line == "\r\n" {
			break
		}
		name, value, ok := strings.Cut(line, ":")
		if ok && strings.EqualFold(name, "content-length") {
			length, _ = strconv.Atoi(strings.TrimSpace(value))
		}
		if ok && strings.EqualFold(name, "transfer-encoding") {
			chunked = strings.Contains(strings.ToLower(value), "chunked")
		}
	}
	if !chunked {
		_, err := io.CopyN(io.Discard, r, int64(length))
		return err == nil
	}
	for {
		line, err := r.ReadString('\n')
		if err != nil {
			return false
		}
		size, err := strconv.ParseInt(strings.TrimSpace(strings.SplitN(line, ";", 2)[0]), 16, 64)
		if err != nil {
			return false
		}
		if _, err := io.CopyN(io.Discard, r, size+2); err != nil {
			return false
		}
		if size == 0 {
			return true
		}
	}
}
