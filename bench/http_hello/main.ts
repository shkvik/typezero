// The server of the HTTP benchmark (bench/http.ps1): "Hello, World!" to
// every request, on the port given.
import { serve } from "tz:http"

const port = parseInt(process.argv[2] ?? "8080") | 0
serve({ port, hostname: "127.0.0.1" }, () => new Response("Hello, World!"))
