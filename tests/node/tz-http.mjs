// "tz:http" for the tests that run under Node too (see preload.cjs): serve
// as tz has it (docs/net.md, section 1), over node:http, or node:https with
// `tls: { cert, key }`, so that a test with a server and its client prints
// the same under both.
import { createServer } from "node:http";
import { createServer as createSecureServer } from "node:https";
import net from "node:net";
import { Readable } from "node:stream";

export function serve(options, handler) {
  const hostname = options.hostname ?? "0.0.0.0";
  const sockets = new Set();
  let stopping = false;
  let resolveFinished;
  const finished = new Promise(resolve => (resolveFinished = resolve));
  const scheme = options.tls ? "https" : "http";
  const listener = async (req, res) => {
    const host = req.headers.host ?? `${hostname}:${server.address().port}`;
    const framed = req.headers["content-length"] !== undefined ||
      req.headers["transfer-encoding"] !== undefined;
    const hasBody = req.method !== "GET" && req.method !== "HEAD" && framed;
    const headers = [];
    for (let i = 0; i < req.rawHeaders.length; i += 2)
      headers.push([req.rawHeaders[i].toLowerCase(), req.rawHeaders[i + 1]]);
    const request = new Request(`${scheme}://${host}${req.url}`, {
      method: req.method,
      headers,
      body: hasBody ? Readable.toWeb(req) : undefined,
      duplex: "half",
    });
    let response;
    try {
      response = await handler(request);
    } catch (e) {
      console.error(e);
      response = new Response("Internal Server Error", { status: 500 });
    }
    const out = {};
    for (const [name, value] of response.headers) {
      if (name === "set-cookie") continue;
      out[name] = value;
    }
    const cookies = response.headers.getSetCookie();
    if (cookies.length) out["set-cookie"] = cookies;
    if (stopping) out.connection = "close";
    res.writeHead(response.status, response.statusText || undefined, out);
    if (response.body && req.method !== "HEAD") {
      for await (const chunk of response.body) res.write(chunk);
    }
    res.end();
  };
  const server = options.tls
    ? createSecureServer({ cert: options.tls.cert, key: options.tls.key }, listener)
    : createServer(listener);
  server.on("connection", socket => {
    sockets.add(socket);
    socket.on("close", () => sockets.delete(socket));
  });
  server.keepAliveTimeout = 5000;
  // Bound at once, so that the port is known as serve returns, as in tz:
  // listen(port, host) would look the host up first, even an address.
  const family = net.isIP(hostname);
  if (family) {
    const handle = net._createServerHandle(hostname, options.port ?? 8000, family);
    if (typeof handle === "number") throw new Error(`listen failed (${handle})`);
    server.listen(handle);
  } else {
    server.listen(options.port ?? 8000, hostname);
  }
  const port = server.address()?.port ?? options.port ?? 8000;
  const stop = () => {
    if (!stopping) {
      stopping = true;
      server.close(() => resolveFinished());
      server.closeIdleConnections();
    }
    return finished;
  };
  if (options.signal) {
    if (options.signal.aborted) stop();
    else options.signal.onabort = () => void stop();
  }
  options.onListen?.({ hostname, port });
  return {
    port,
    hostname,
    get url() {
      const host = hostname === "0.0.0.0" || hostname === "::" ? "localhost" : hostname;
      return new URL(`${scheme}://${host.includes(":") ? `[${host}]` : host}:${port}/`);
    },
    finished,
    stop,
  };
}
