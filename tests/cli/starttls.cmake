# STARTTLS in a program without `transport` (docs/net.md, section 2): a call of
# Socket.startTls() is enough for the program to get TLS. The server, a program
# of its own with TLS, prints its port; the client reads it, connects over
# plain TCP, starts TLS and asks for a page over it.
include(${SOURCE}/../cli.cmake)

write(server.tz [[
import { readFileSync } from "node:fs"
import { join } from "node:path"
import { Server } from "tz:http"

const dir = process.env.TLS_DIR ?? "."
const cert = readFileSync(join(dir, "localhost.pem"), "utf8")
const key = readFileSync(join(dir, "key.pem"), "utf8")
if (cert.isErr() || key.isErr()) process.exit(1)
const answered = Promise.withResolvers<void>()
const server = new Server(
  { port: 0, hostname: "127.0.0.1", transport: { secure: true, cert: cert.value, key: key.value } },
  request => {
    answered.resolve()
    return new Response("tls ok")
  },
)
const listening = server.listen()
if (listening.isErr()) process.exit(1)
console.log(listening.value.port)
const guard = setTimeout(() => process.exit(2), 20000)
await answered.promise
await listening.value.stop()
clearTimeout(guard)
]])
write(client.tz [[
import { Socket } from "node:net"
import { readLine } from "tz:io"

const port = parseInt(readLine() ?? "").unwrapOr(0)
const socket = new Socket({ host: "127.0.0.1", port: port })
const connected = await socket.connect()
if (connected.isErr()) {
  console.log("connect:", connected.error.kind, connected.error.code)
  process.exit(1)
}
const secured = await socket.startTls("localhost")
if (secured.isErr()) {
  console.log("startTls:", secured.error.kind, secured.error.code)
  process.exit(1)
}
socket.write("GET / HTTP/1.1\r\nhost: localhost\r\nconnection: close\r\n\r\n")
let text = ""
for await (const chunk of socket) {
  if (chunk.isErr()) break
  text += chunk.value.toString()
}
console.log("over tls:", text.includes("tls ok"))
]])
tz(build server.tz)
tz(build client.tz)
if(WIN32)
  set(exe ".exe")
else()
  set(exe "")
endif()
set(ENV{TLS_DIR} ${SOURCE}/../tls)
set(ENV{NODE_EXTRA_CA_CERTS} ${SOURCE}/../tls/ca.pem)
execute_process(COMMAND ${WORK}/server${exe} COMMAND ${WORK}/client${exe}
  WORKING_DIRECTORY ${WORK} TIMEOUT 60
  OUTPUT_VARIABLE out ERROR_VARIABLE err RESULTS_VARIABLE codes)
normalize(out "${out}")
normalize(err "${err}")
if(NOT out STREQUAL "over tls: true\n" OR NOT codes STREQUAL "0;0")
  message(FATAL_ERROR "the client printed:\n${out}\nexit codes: ${codes}\nstderr:\n${err}")
endif()
if(err MATCHES "leaked")
  message(FATAL_ERROR "memory leak:\n${err}")
endif()
