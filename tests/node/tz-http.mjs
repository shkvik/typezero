// "tz:http" for the tests that run under Node too (see preload.cjs): Server
// as tz has it (docs/net.md, section 1), over node:http, or node:https with
// `tls: { cert, key }`, so that a test with a server and its client prints
// the same under both. listen gives a Result, as in tz: a port that is taken
// is an Err there.
import { createServer } from "node:http";
import { createServer as createSecureServer } from "node:https";
import net from "node:net";
import { Readable } from "node:stream";

export class Server {
  port = 0;
  hostname = "";
  url = undefined;
  #handler;
  #server = undefined;
  #listened = false;
  #stopping = false;
  #resolveFinished;

  constructor(handler) {
    this.#handler = handler;
    this.finished = new Promise(resolve => (this.#resolveFinished = resolve));
  }

  listen(options) {
    if (this.#listened)
      return err({
        code: "ERR_SERVER_ALREADY_LISTEN",
        message: "Listen method has been called more than once without closing.",
      });
    const hostname = options.hostname ?? "0.0.0.0";
    const sockets = new Set();
    const scheme = options.tls ? "https" : "http";
    const listener = async (req, res) => {
      const host = req.headers.host ?? `${hostname}:${this.port}`;
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
      const response = await this.#handler(request);
      const out = {};
      for (const [name, value] of response.headers) {
        if (name === "set-cookie") continue;
        out[name] = value;
      }
      const cookies = response.headers.getSetCookie();
      if (cookies.length) out["set-cookie"] = cookies;
      if (this.#stopping) out.connection = "close";
      res.writeHead(response.status, response.statusText || undefined, out);
      if (response.body && req.method !== "HEAD") {
        for await (const chunk of response.body) {
          if (chunk.isErr()) break;
          res.write(chunk.value);
        }
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
    // Bound at once, so that the port is known as listen returns, as in tz:
    // listen(port, host) would look the host up first, even an address.
    const family = net.isIP(hostname);
    if (family) {
      const handle = net._createServerHandle(hostname, options.port ?? 8000, family);
      if (typeof handle === "number") return err({ code: "listen", message: `listen failed (${handle})` });
      server.listen(handle);
    } else {
      server.listen(options.port ?? 8000, hostname);
    }
    this.#listened = true;
    this.#server = server;
    this.port = server.address()?.port ?? options.port ?? 8000;
    this.hostname = hostname;
    const shown = hostname === "0.0.0.0" || hostname === "::" ? "localhost" : hostname;
    this.url = new URL(`${scheme}://${shown.includes(":") ? `[${shown}]` : shown}:${this.port}/`);
    if (options.signal) {
      if (options.signal.aborted) this.stop();
      else options.signal.onabort = () => void this.stop();
    }
    options.onListen?.({ hostname, port: this.port });
    return ok(this.url);
  }

  stop() {
    const server = this.#server;
    if (server === undefined) return Promise.resolve();
    if (!this.#stopping) {
      this.#stopping = true;
      server.close(() => this.#resolveFinished());
      server.closeIdleConnections();
    }
    return this.finished;
  }
}
