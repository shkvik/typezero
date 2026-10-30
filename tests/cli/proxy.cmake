# fetch through a proxy (docs/net.md, section 2): HTTP_PROXY from the
# environment for http:// (the request goes to the proxy with the full
# address), proxy: { enabled, url } in the code for https:// (a CONNECT
# tunnel, then TLS with the server itself), a proxy that asks for a login
# (407) and one given with it, a proxy that refuses (403), a proxy URL that
# is not one, localhost always past the proxy. Node.js plays the proxy on
# 127.0.0.1, and sends every host to 127.0.0.1.
include(${SOURCE}/../cli.cmake)
if(NOT NODE)
  skip("Node.js is not installed")
endif()

string(RANDOM LENGTH 4 ALPHABET 0123456789 suffix)
math(EXPR proxy_port "20000 + ${suffix}")
math(EXPR auth_port "30000 + ${suffix}")
get_filename_component(tls_dir ${SOURCE}/../tls ABSOLUTE)

write(proxy.mjs [[
import http from "node:http";
import net from "node:net";
const [port, authPort] = process.argv.slice(2).map(Number);
const servers = [];
function proxy(needsLogin) {
  const server = http.createServer((req, res) => {
    if (!req.url.startsWith("http://")) {
      res.end("bye");
      if (req.url == "/quit") for (const s of servers) s.close();
      return;
    }
    const target = new URL(req.url);
    const upstream = http.request({ host: "127.0.0.1", port: target.port, path: target.pathname,
                                    method: req.method, headers: { ...req.headers, "x-via": "proxy" } },
                                  answer => {
      res.writeHead(answer.statusCode, answer.headers);
      answer.pipe(res);
    });
    req.pipe(upstream);
  });
  server.on("connect", (req, socket, head) => {
    const login = req.headers["proxy-authorization"];
    if (needsLogin && login !== "Basic " + Buffer.from("ann:secret").toString("base64")) {
      socket.end("HTTP/1.1 407 Proxy Authentication Required\r\ncontent-length: 0\r\n\r\n");
      return;
    }
    if (req.url.startsWith("blocked.test")) {
      socket.end("HTTP/1.1 403 Forbidden\r\ncontent-length: 0\r\n\r\n");
      return;
    }
    const targetPort = Number(req.url.split(":").pop());
    const upstream = net.connect(targetPort, "127.0.0.1", () => {
      socket.write("HTTP/1.1 200 Connection Established\r\n\r\n");
      upstream.write(head);
      upstream.pipe(socket);
      socket.pipe(upstream);
    });
    upstream.on("error", () => socket.destroy());
    socket.on("error", () => upstream.destroy());
  });
  servers.push(server);
  return server;
}
proxy(false).listen(port, "127.0.0.1");
proxy(true).listen(authPort, "127.0.0.1");
]])
write(client.tz "import { Server } from \"tz:http\"
import { readFileSync } from \"node:fs\"

const key = must(readFileSync(\"${tls_dir}/key.pem\", \"utf8\"))
const cert = must(readFileSync(\"${tls_dir}/localhost.pem\", \"utf8\"))
const plain = must(new Server({ port: 0, hostname: \"127.0.0.1\" }, req => new Response(`plain \${req.headers.get(\"x-via\") ?? \"direct\"}`)).listen())
const secure = must(new Server({ port: 0, hostname: \"127.0.0.1\", transport: { secure: true, cert, key } }, () => new Response(\"secure\")).listen())
const proxyUrl = \"http://127.0.0.1:${proxy_port}\"
for (let tries = 0; ; tries++) {
  const probe = await fetch(`\${proxyUrl}/probe`, { proxy: { enabled: false } })
  if (probe.isOk() || tries == 100) break
  await new Promise<void>(resolve => setTimeout(resolve, 50))
}

async function show(what: string, url: string, init?: RequestInit) {
  const r = await fetch(url, init)
  if (r.isErr()) {
    const e = r.error
    console.log(what, e.kind, e.code, e.message.replaceAll(String(${proxy_port}), \"PROXY\").replaceAll(String(${auth_port}), \"AUTH\").replaceAll(String(secure.port), \"PORT\"))
    return
  }
  console.log(what, r.value.status, must(await r.value.text()))
}

process.env.HTTP_PROXY = proxyUrl
await show(\"env http\", `http://example.test:\${plain.port}/`)
await show(\"localhost\", `http://localhost:\${plain.port}/`)
await show(\"no proxy\", `http://example.test:\${plain.port}/`, { proxy: { enabled: false }, timeout: 3000 })
process.env.HTTP_PROXY = \"\"
await show(\"https tunnel\", `https://localhost:\${secure.port}/`, { proxy: { enabled: true, url: proxyUrl } })
await show(\"login asked\", `https://localhost:\${secure.port}/`, { proxy: { enabled: true, url: \"http://127.0.0.1:${auth_port}\" } })
await show(\"login given\", `https://localhost:\${secure.port}/`, { proxy: { enabled: true, url: \"http://ann:secret@127.0.0.1:${auth_port}\" } })
await show(\"refused\", `https://blocked.test:\${secure.port}/`, { proxy: { enabled: true, url: proxyUrl } })
process.env.HTTPS_PROXY = \"proxy.corp:8080\"
await show(\"bad env\", `https://example.test:\${secure.port}/`)
process.env.HTTPS_PROXY = \"\"
const quit = await fetch(`\${proxyUrl}/quit`, { proxy: { enabled: false } })
if (quit.isErr()) console.log(\"quit:\", quit.error.message)
await plain.stop()
await secure.stop()

function must<T, E>(r: Result<T, E>): T {
  if (r.isErr()) {
    console.log(r.error)
    process.exit(1)
  }
  return r.value
}
")
tz(build -o client.exe client.tz)
set(ENV{NODE_EXTRA_CA_CERTS} ${tls_dir}/ca.pem)
execute_process(COMMAND ${NODE} ${WORK}/proxy.mjs ${proxy_port} ${auth_port} COMMAND ${WORK}/client.exe
  WORKING_DIRECTORY ${WORK}
  OUTPUT_VARIABLE out ERROR_VARIABLE err RESULTS_VARIABLE codes)
normalize(out "${out}")
string(REGEX REPLACE "no proxy [A-Z_]+ [A-Z_]+ [^\n]*" "no proxy (direct, no such host)" out "${out}")
set(expected [[
env http 200 plain proxy
localhost 200 plain direct
no proxy (direct, no such host)
https tunnel 200 secure
login asked FETCH_ERROR_PROXY ERR_PROXY_AUTH_REQUIRED the proxy 127.0.0.1:AUTH asks for a login (407): put user and password into its URL, http://user:password@host:port
login given 200 secure
refused FETCH_ERROR_PROXY ERR_PROXY_REFUSED the proxy 127.0.0.1:PROXY refused to connect to blocked.test:PORT (403)
bad env FETCH_ERROR_PROXY ERR_INVALID_PROXY_URL HTTPS_PROXY is a proxy.corp: proxy, and only http: and https: proxies work
]])
if(NOT codes STREQUAL "0;0" OR NOT out STREQUAL expected)
  message(FATAL_ERROR "exit codes ${codes}\n--- expected:\n${expected}\n--- actual:\n${out}"
                      "\nstderr:\n${err}")
endif()
