// "tz:http" for the tests that run under Node too (see preload.cjs): Server
// as tz has it (docs/net.md, section 1), over node:http, or node:https with
// `transport: { secure: true, cert, key }`, so that a test with a server and
// its client prints the same under both. new Server(options, handler) keeps
// the options; listen() gives a Result with what listens (url, hostname,
// port, stop()), as in tz: a port that is taken is an Err there.
import { createServer } from "node:http";
import { createServer as createSecureServer } from "node:https";
import net from "node:net";
import { Readable } from "node:stream";

export class Server {
  #options;
  #handler;

  constructor(options, handler) {
    this.#options = options;
    this.#handler = handler;
  }

  listen() {
    const options = this.#options;
    const handler = this.#handler;
    const hostname = options.hostname ?? "0.0.0.0";
    const transport = options.transport ?? { secure: false };
    const scheme = transport.secure ? "https" : "http";
    const listening = new Listening();
    const listener = async (req, res) => {
      const host = req.headers.host ?? `${hostname}:${listening.port}`;
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
      const client = { address: req.socket.remoteAddress, port: req.socket.remotePort };
      const response = await handler(request, client);
      const out = {};
      for (const [name, value] of response.headers) {
        if (name === "set-cookie") continue;
        out[name] = value;
      }
      const cookies = response.headers.getSetCookie();
      if (cookies.length) out["set-cookie"] = cookies;
      if (listening.stopping) out.connection = "close";
      res.writeHead(response.status, response.statusText || undefined, out);
      if (response.body && req.method !== "HEAD") {
        for await (const chunk of response.body) {
          if (chunk.isErr()) break;
          res.write(chunk.value);
        }
      }
      res.end();
    };
    const server = transport.secure
      ? createSecureServer({ cert: transport.cert, key: transport.key }, listener)
      : createServer(listener);
    server.keepAliveTimeout = options.keepAliveTimeout ?? 5000;
    server.headersTimeout = options.headersTimeout ?? 60000;
    // Bound at once, so that the port is known as listen returns, as in tz:
    // listen(port, host) would look the host up first, even an address.
    const family = net.isIP(hostname);
    const port = options.port ?? 8000;
    if (family) {
      const handle = net._createServerHandle(hostname, port, family);
      if (typeof handle === "number")
        return err({ kind: "SERVER", code: "EADDRINUSE", message: `listen failed (${handle})`,
                     errno: handle, syscall: "listen", address: hostname, port });
      server.listen(handle);
    } else {
      server.listen(port, hostname);
    }
    listening.server = server;
    listening.port = server.address()?.port ?? port;
    listening.hostname = hostname;
    const shown = hostname === "0.0.0.0" || hostname === "::" ? "localhost" : hostname;
    listening.url = new URL(`${scheme}://${shown.includes(":") ? `[${shown}]` : shown}:${listening.port}/`);
    listening.finished = new Promise(resolve => (listening.resolve = resolve));
    return ok(listening);
  }
}

class Listening {
  stop() {
    if (!this.stopping) {
      this.stopping = true;
      this.server.close(() => this.resolve());
      this.server.closeIdleConnections();
    }
    return this.finished;
  }
}
