// The server of the HTTP/2 benchmark (bench/http2.ps1): "Hello, World!" to
// every request over HTTP/2 without TLS (h2c, prior knowledge), on the port
// given, as main.tz.
package main

import (
	"net/http"
	"os"

	"golang.org/x/net/http2"
	"golang.org/x/net/http2/h2c"
)

func main() {
	port := "8080"
	if len(os.Args) > 1 {
		port = os.Args[1]
	}
	hello := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/plain;charset=UTF-8")
		w.Write([]byte("Hello, World!"))
	})
	server := &http.Server{Addr: "127.0.0.1:" + port, Handler: h2c.NewHandler(hello, &http2.Server{})}
	server.ListenAndServe()
}
