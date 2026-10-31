# HTTP/2 of tz:http (docs/net.md, section 1): a server with
# http2: { enabled: true, cleartext: true } takes HTTP/2 without TLS from a
# client that starts with it (h2c), and an https server takes HTTP/2 by ALPN
# and HTTP/1.1 from clients that ask for it; the handler is the same, and
# client.protocol tells which came. Many requests go at once on one
# connection; bodies come both ways; a stream body is sent in parts; stop()
# ends the connections with GOAWAY. Node.js's node:http2 plays the client, on
# 127.0.0.1.
include(${SOURCE}/../cli.cmake)
if(NOT NODE)
  skip("Node.js is not installed")
endif()

string(RANDOM LENGTH 4 ALPHABET 0123456789 suffix)
math(EXPR plain_port "21000 + ${suffix}")
math(EXPR tls_port "31000 + ${suffix}")
set(tls ${SOURCE}/../tls)

write(server.tz "
import { readFileSync } from \"node:fs\"
import { Handler, Listening, Server } from \"tz:http\"

const enc = new TextEncoder()
const cert = readFileSync(\"${tls}/localhost.pem\", \"utf8\")
const key = readFileSync(\"${tls}/key.pem\", \"utf8\")
if (cert.isErr() || key.isErr()) process.exit(1)
const running: Listening[] = []
const handler: Handler = async (req, client) => {
  const url = new URL(req.url)
  switch (url.pathname()) {
    case \"/hello\":
      return new Response(`hello over \${client.protocol}: \${req.method} \${url.pathname()}\${url.search()}`,
                          { headers: { \"x-test\": \"yes\" } })
    case \"/echo\": {
      const text = await req.text()
      return new Response(text.isOk() ? `got \${text.value}` : `failed \${text.error.code}`)
    }
    case \"/stream\":
      return Response.stream(new ReadableStream<Uint8Array>({
        start: c => {
          c.enqueue(enc.encode(\"one \"))
          c.enqueue(enc.encode(\"two\"))
          c.close()
        },
      }))
    case \"/empty\":
      return new Response(undefined, { status: 204 })
    case \"/host\":
      return new Response(req.headers.get(\"host\") ?? \"no host\")
    case \"/stop\":
      setTimeout(() => {
        for (const listening of running) void listening.stop()
      }, 10)
      return new Response(\"stopping\")
  }
  return new Response(\"not found\", { status: 404 })
}
const plain = new Server({ port: ${plain_port}, hostname: \"127.0.0.1\",
                           http2: { enabled: true, cleartext: true } }, handler).listen()
const secure = new Server({ port: ${tls_port}, hostname: \"127.0.0.1\",
                            transport: { secure: true, cert: cert.value, key: key.value } },
                          handler).listen()
if (plain.isErr() || secure.isErr()) process.exit(1)
running.push(plain.value)
running.push(secure.value)
")

write(client.mjs [[
import fs from "node:fs";
import http2 from "node:http2";
import https from "node:https";

const [plainPort, tlsPort, caPath] = process.argv.slice(2);
const ca = fs.readFileSync(caPath);

function ask(session, path, method = "GET", body) {
  return new Promise((resolve, reject) => {
    const stream = session.request({ ":method": method, ":path": path });
    let status = 0;
    let test = "";
    const parts = [];
    stream.on("response", headers => {
      status = headers[":status"];
      test = headers["x-test"] ?? "";
    });
    stream.on("data", chunk => parts.push(chunk));
    stream.on("end", () => resolve({ status, test, body: Buffer.concat(parts).toString() }));
    stream.on("error", reject);
    if (body !== undefined) stream.end(body);
    else stream.end();
  });
}

async function connect(url, options) {
  for (let tries = 0; ; tries++) {
    try {
      const session = http2.connect(url, options);
      await new Promise((resolve, reject) => {
        session.once("connect", resolve);
        session.once("error", reject);
      });
      return session;
    } catch (e) {
      if (tries == 100) throw e;
      await new Promise(resolve => setTimeout(resolve, 50));
    }
  }
}

const plain = await connect(`http://127.0.0.1:${plainPort}`);
const hello = await ask(plain, "/hello?a=1");
console.log("h2c", hello.status, hello.test, hello.body);
const many = await Promise.all(Array.from({ length: 50 }, () => ask(plain, "/hello")));
console.log("h2c at once", many.length, many.every(r => r.status == 200 && r.body == "hello over h2: GET /hello"));
const echo = await ask(plain, "/echo", "POST", "x".repeat(100000));
console.log("h2c echo", echo.status, echo.body.length);
const streamed = await ask(plain, "/stream");
console.log("h2c stream", streamed.status, streamed.body);
const empty = await ask(plain, "/empty");
console.log("h2c empty", empty.status, JSON.stringify(empty.body));
const host = await ask(plain, "/host");
console.log("h2c host", host.status, host.body == `127.0.0.1:${plainPort}`);
const head = await ask(plain, "/hello", "HEAD");
console.log("h2c head", head.status, JSON.stringify(head.body));

const secure = await connect(`https://localhost:${tlsPort}`, { ca });
console.log("tls alpn", secure.alpnProtocol);
const overTls = await ask(secure, "/hello");
console.log("tls", overTls.status, overTls.body);
const old = await new Promise((resolve, reject) => {
  https.get({ host: "localhost", port: tlsPort, path: "/hello", ca }, res => {
    const parts = [];
    res.on("data", chunk => parts.push(chunk));
    res.on("end", () => resolve(`${res.statusCode} ${Buffer.concat(parts)}`));
  }).on("error", reject);
});
console.log("tls http/1.1", old);

const stop = await ask(plain, "/stop");
console.log("stop", stop.body);
const closed = await new Promise(resolve => {
  plain.once("goaway", () => resolve("goaway"));
  plain.once("close", () => resolve("closed"));
});
console.log("h2c after stop", closed);
secure.close();
plain.close();
]])

tz(build -o server.exe server.tz)
execute_process(COMMAND ${WORK}/server.exe COMMAND ${NODE} ${WORK}/client.mjs ${plain_port} ${tls_port} ${tls}/ca.pem
  WORKING_DIRECTORY ${WORK}
  OUTPUT_VARIABLE out ERROR_VARIABLE err RESULTS_VARIABLE codes TIMEOUT 60)
normalize(out "${out}")
set(expected [[
h2c 200 yes hello over h2: GET /hello?a=1
h2c at once 50 true
h2c echo 200 100004
h2c stream 200 one two
h2c empty 204 ""
h2c host 200 true
h2c head 200 ""
tls alpn h2
tls 200 hello over h2: GET /hello
tls http/1.1 200 hello over http/1.1: GET /hello
stop stopping
h2c after stop goaway
]])
if(NOT codes STREQUAL "0;0" OR NOT out STREQUAL expected)
  message(FATAL_ERROR "exit codes ${codes}\n--- expected:\n${expected}\n--- actual:\n${out}"
                      "\nstderr:\n${err}")
endif()
