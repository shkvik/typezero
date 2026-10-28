# The server answers 400 to a request whose Host, or the host of an absolute
# target, is no host of a URL, so req.url always parses (docs/net.md,
# section 1): a client in Node.js sends raw requests to a server that tz
# built, on 127.0.0.1.
include(${SOURCE}/../cli.cmake)
if(NOT NODE)
  skip("Node.js is not installed")
endif()

string(RANDOM LENGTH 4 ALPHABET 0123456789 suffix)
math(EXPR port "20000 + ${suffix}")
write(server.tz "import { Server } from \"tz:http\"
const stop = new AbortController()
const server = new Server(req => {
  const url = URL.parse(req.url)
  if (url.isOk() && url.value.pathname == \"/stop\") stop.abort()
  return new Response(`\${req.url} \${url.isOk()}`)
})
const started = server.listen({ port: ${port}, hostname: \"127.0.0.1\", signal: stop.signal })
if (started.isErr()) process.exit(1)
")
write(client.mjs [[
import net from "node:net";
const port = Number(process.argv[2]);
const hosts = ["example.com", "a b", "x.123", "1.2.3.4", "1.2.3.999", "0x7f.1", "[::1]",
               "[::1::2]", "[1:2:3:4:5:6:7:8]", "[::ffff:1.2.3.4]", "[::1.02.3.4]", "h:99999",
               "h:080", "user@h", "h%41", "\xfc.com", "", "a..b", "a.", "1.2.3.4.5"];
const targets = ["/x", "http://good/x", "https://[::1]:80/y", "http://a@b/x", "http://[::/x"];
function send(host, target) {
  return new Promise(resolve => {
    const s = net.connect(port, "127.0.0.1", () => {
      s.write(`GET ${target} HTTP/1.1\r\nHost: ${host}\r\nConnection: close\r\n\r\n`, "latin1");
    });
    let out = "";
    s.on("data", d => (out += d.toString("latin1")));
    s.on("end", () => {
      const lines = out.split("\r\n");
      resolve(lines[0].slice(9) + (lines[0].includes("200") ? ": " + lines[lines.length - 1] : ""));
    });
    s.on("error", e => resolve(e.code));
  });
}
for (let tries = 0; ; tries++) {
  const up = await new Promise(resolve => {
    const s = net.connect(port, "127.0.0.1", () => (s.end(), resolve(true)));
    s.on("error", () => resolve(false));
  });
  if (up) break;
  if (tries == 100) throw new Error("the server did not start");
  await new Promise(resolve => setTimeout(resolve, 50));
}
for (const h of hosts) console.log(JSON.stringify(h), await send(h, "/x"));
for (const t of targets) console.log(JSON.stringify(t), await send("ok.com", t));
await send("ok.com", "/stop");
]])
tz(build -o server.exe server.tz)
execute_process(COMMAND ${WORK}/server.exe COMMAND ${NODE} ${WORK}/client.mjs ${port}
  WORKING_DIRECTORY ${WORK}
  OUTPUT_VARIABLE out ERROR_VARIABLE err RESULTS_VARIABLE codes)
normalize(out "${out}")
set(expected [[
"example.com" 200 OK: http://example.com/x true
"a b" 400 Bad Request
"x.123" 400 Bad Request
"1.2.3.4" 200 OK: http://1.2.3.4/x true
"1.2.3.999" 400 Bad Request
"0x7f.1" 200 OK: http://0x7f.1/x true
"[::1]" 200 OK: http://[::1]/x true
"[::1::2]" 400 Bad Request
"[1:2:3:4:5:6:7:8]" 200 OK: http://[1:2:3:4:5:6:7:8]/x true
"[::ffff:1.2.3.4]" 200 OK: http://[::ffff:1.2.3.4]/x true
"[::1.02.3.4]" 400 Bad Request
"h:99999" 400 Bad Request
"h:080" 200 OK: http://h:080/x true
"user@h" 400 Bad Request
"h%41" 400 Bad Request
"ü.com" 400 Bad Request
"" 400 Bad Request
"a..b" 200 OK: http://a..b/x true
"a." 200 OK: http://a./x true
"1.2.3.4.5" 400 Bad Request
"/x" 200 OK: http://ok.com/x true
"http://good/x" 200 OK: http://good/x true
"https://[::1]:80/y" 200 OK: https://[::1]:80/y true
"http://a@b/x" 400 Bad Request
"http://[::/x" 400 Bad Request
]])
if(NOT codes STREQUAL "0;0" OR NOT out STREQUAL expected)
  message(FATAL_ERROR "exit codes ${codes}\n--- expected:\n${expected}\n--- actual:\n${out}"
                      "\nstderr:\n${err}")
endif()
