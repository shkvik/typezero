// The server of the HTTP benchmark under node (bench/http.ps1), as main.tz.
import { createServer } from "node:http";

const port = Number(process.argv[2] ?? 8080);
createServer((req, res) => {
  res.writeHead(200, { "content-type": "text/plain;charset=UTF-8" });
  res.end("Hello, World!");
}).listen(port, "127.0.0.1");
