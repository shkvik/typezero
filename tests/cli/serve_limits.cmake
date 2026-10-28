# The limits a server keeps by itself (docs/net.md, section 1): the head of
# a request that comes too slowly is answered with 408 (headersTimeout), a
# connection that waits too long for its next request is closed
# (keepAliveTimeout), and the handler learns who sent the request. A client
# in Node.js talks to a server that tz built, on 127.0.0.1.
include(${SOURCE}/../cli.cmake)
if(NOT NODE)
  skip("Node.js is not installed")
endif()

string(RANDOM LENGTH 4 ALPHABET 0123456789 suffix)
math(EXPR port "20000 + ${suffix}")
write(server.tz "import { Server } from \"tz:http\"
const stopAsked = Promise.withResolvers<void>()
const options = {
  port: ${port},
  hostname: \"127.0.0.1\",
  keepAliveTimeout: 300,
  headersTimeout: 400,
}
const server = new Server(options, (req, client) => {
  if (new URL(req.url).pathname() == \"/stop\") stopAsked.resolve()
  return new Response(`\${client.address} \${client.port > 0}`)
})
const started = server.listen()
if (started.isErr()) process.exit(1)
await stopAsked.promise
await started.value.stop()
")
write(client.mjs [[
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
  });
}
for (let tries = 0; ; tries++) {
  const up = await connect().then(s => (s.end(), true), () => false);
  if (up) break;
  if (tries == 100) throw new Error("the server did not start");
  await new Promise(resolve => setTimeout(resolve, 50));
}

const asked = await connect();
const answer = collect(asked);
asked.write("GET /who HTTP/1.1\r\nHost: ok\r\n\r\n");
const { out, ms } = await answer;
const body = out.split("\r\n\r\n")[1];
console.log("client:", body);
console.log("idle connection closed:", ms >= 200 && ms < 3000);

const slow = await connect();
const cut = collect(slow);
slow.write("GET /slow HTTP/1.1\r\nHo");
const late = await cut;
console.log("slow head:", late.out.split("\r\n")[0], late.ms >= 300 && late.ms < 3000);

const last = await connect();
const done = collect(last);
last.write("GET /stop HTTP/1.1\r\nHost: ok\r\nConnection: close\r\n\r\n");
await done;
]])
tz(build -o server.exe server.tz)
execute_process(COMMAND ${WORK}/server.exe COMMAND ${NODE} ${WORK}/client.mjs ${port}
  WORKING_DIRECTORY ${WORK}
  OUTPUT_VARIABLE out ERROR_VARIABLE err RESULTS_VARIABLE codes)
normalize(out "${out}")
set(expected [[
client: 127.0.0.1 true
idle connection closed: true
slow head: HTTP/1.1 408 Request Timeout true
]])
if(NOT codes STREQUAL "0;0" OR NOT out STREQUAL expected)
  message(FATAL_ERROR "exit codes ${codes}\n--- expected:\n${expected}\n--- actual:\n${out}"
                      "\nstderr:\n${err}")
endif()
