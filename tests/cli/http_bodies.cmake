# Bodies that go wrong (docs/net.md, sections 1-3): fetch tells a body in
# chunks that are not chunked encoding (BODY_ERROR_MALFORMED, ERR_INVALID_CHUNKED) from a
# body the server cut off (BODY_ERROR_NETWORK, ERR_CLOSED); a server answers a request
# body in bad chunks with 400 and its handler gets ERR_INVALID_CHUNKED; a
# request whose body comes slower than requestTimeout is answered with 408 and
# its handler gets BODY_ERROR_TIMEOUT. Node.js plays the other side over raw sockets,
# on 127.0.0.1.
include(${SOURCE}/../cli.cmake)
if(NOT NODE)
  skip("Node.js is not installed")
endif()

string(RANDOM LENGTH 4 ALPHABET 0123456789 suffix)
math(EXPR raw_port "20000 + ${suffix}")
math(EXPR tz_port "30000 + ${suffix}")

write(raw-server.mjs [[
import net from "node:net";
const port = Number(process.argv[2]);
let served = 0;
const server = net.createServer(socket => {
  socket.once("data", data => {
    const path = data.toString("latin1").split(" ")[1];
    if (path === "/probe")
      socket.end("HTTP/1.1 200 OK\r\ncontent-length: 2\r\nconnection: close\r\n\r\nok");
    else if (path === "/bad")
      socket.end("HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\nzz\r\nhello\r\n0\r\n\r\n");
    else
      socket.end("HTTP/1.1 200 OK\r\ncontent-length: 100\r\n\r\nshort");
    if (++served == 3) server.close();
  });
});
server.listen(port, "127.0.0.1");
]])
write(client.tz "
const base = `http://127.0.0.1:${raw_port}`
for (let tries = 0; ; tries++) {
  const probe = await fetch(`\${base}/probe`)
  if (probe.isOk() || tries == 100) break
  await new Promise<void>(resolve => setTimeout(resolve, 50))
}
for (const path of [\"/bad\", \"/cut\"]) {
  const r = await fetch(`\${base}\${path}`)
  if (r.isErr()) {
    console.log(path, r.error.kind, r.error.code)
    continue
  }
  const text = await r.value.text()
  if (text.isErr()) console.log(path, text.error.kind, text.error.code, text.error.message.replaceAll(String(${raw_port}), \"PORT\"))
  else console.log(path, \"read\", text.value)
}
")
tz(build -o client.exe client.tz)
execute_process(COMMAND ${NODE} ${WORK}/raw-server.mjs ${raw_port} COMMAND ${WORK}/client.exe
  WORKING_DIRECTORY ${WORK}
  OUTPUT_VARIABLE out ERROR_VARIABLE err RESULTS_VARIABLE codes)
normalize(out "${out}")
set(expected [[
/bad BODY_ERROR_MALFORMED ERR_INVALID_CHUNKED the body is not valid chunked encoding
/cut BODY_ERROR_NETWORK ERR_CLOSED 127.0.0.1:PORT closed the connection before the end of the body
]])
if(NOT codes STREQUAL "0;0" OR NOT out STREQUAL expected)
  message(FATAL_ERROR "client: exit codes ${codes}\n--- expected:\n${expected}\n--- actual:\n"
                      "${out}\nstderr:\n${err}")
endif()

write(server.tz "import { Server } from \"tz:http\"
const stopAsked = Promise.withResolvers<void>()
const server = new Server({ port: ${tz_port}, hostname: \"127.0.0.1\", requestTimeout: 300 }, async req => {
  const path = new URL(req.url).pathname()
  if (path == \"/stop\") {
    stopAsked.resolve()
    return new Response(\"bye\")
  }
  const text = await req.text()
  if (text.isErr()) console.log(path, text.error.kind, text.error.code, text.error.message)
  else console.log(path, \"read\", text.value)
  return new Response(\"ok\")
})
const started = server.listen()
if (started.isErr()) process.exit(1)
await stopAsked.promise
await started.value.stop()
")
write(slow-client.mjs [[
import net from "node:net";
const port = Number(process.argv[2]);
function connect() {
  return new Promise((resolve, reject) => {
    const s = net.connect(port, "127.0.0.1", () => resolve(s));
    s.on("error", reject);
  });
}
function collect(s) {
  return new Promise(resolve => {
    let out = "";
    const started = Date.now();
    s.on("data", d => (out += d.toString("latin1")));
    s.on("close", () => resolve({ out, ms: Date.now() - started }));
    s.on("error", () => {});
  });
}
for (let tries = 0; ; tries++) {
  const up = await connect().then(s => (s.end(), true), () => false);
  if (up) break;
  if (tries == 100) throw new Error("the server did not start");
  await new Promise(resolve => setTimeout(resolve, 50));
}
const slow = await connect();
const slowDone = collect(slow);
slow.write("POST /slow HTTP/1.1\r\nHost: ok\r\nContent-Length: 10\r\n\r\nabc");
const late = await slowDone;
console.error("slow body:", late.out.split("\r\n")[0], late.ms >= 200 && late.ms < 3000);

const bad = await connect();
const badDone = collect(bad);
bad.write("POST /chunks HTTP/1.1\r\nHost: ok\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nhello\r\n0\r\n\r\n");
const refused = await badDone;
console.error("bad chunks:", refused.out.split("\r\n")[0]);

const last = await connect();
const done = collect(last);
last.write("GET /stop HTTP/1.1\r\nHost: ok\r\nConnection: close\r\n\r\n");
await done;
]])
tz(build -o server.exe server.tz)
execute_process(COMMAND ${NODE} ${WORK}/slow-client.mjs ${tz_port} COMMAND ${WORK}/server.exe
  WORKING_DIRECTORY ${WORK}
  OUTPUT_VARIABLE out ERROR_VARIABLE err RESULTS_VARIABLE codes)
normalize(out "${out}")
normalize(err "${err}")
set(expected [[
/slow BODY_ERROR_TIMEOUT TIMEOUT_ERR the request did not come in full within 300 ms (requestTimeout)
/chunks BODY_ERROR_MALFORMED ERR_INVALID_CHUNKED the body is not valid chunked encoding
]])
set(expected_err [[
slow body: HTTP/1.1 408 Request Timeout true
bad chunks: HTTP/1.1 400 Bad Request
]])
if(NOT codes STREQUAL "0;0" OR NOT out STREQUAL expected OR NOT err STREQUAL expected_err)
  message(FATAL_ERROR "server: exit codes ${codes}\n--- expected:\n${expected}${expected_err}"
                      "\n--- actual:\n${out}\nstderr:\n${err}")
endif()
