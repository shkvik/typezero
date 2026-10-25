// The server of the HTTP benchmark (bench/http.ps1): "Hello, World!" to
// every request, on the port given, as main.tz.
package main

import (
	"net/http"
	"os"
)

func main() {
	port := "8080"
	if len(os.Args) > 1 {
		port = os.Args[1]
	}
	http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/plain;charset=UTF-8")
		w.Write([]byte("Hello, World!"))
	})
	http.ListenAndServe("127.0.0.1:"+port, nil)
}
