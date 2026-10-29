// Runs before every `// same-as-node` test under Node (see check.cmake): it
// gives Node what tz has built in and Node has not (docs/io.md).
//
// - SystemError: Node has no global of that name, only the errors of the
//   system it describes. Here `e instanceof SystemError` holds for an Error
//   with a string `code` and `syscall`, which those errors have, and for
//   the errors of TLS, which tz gives as SystemError too (docs/net.md,
//   section 4) and node as an Error with a code only: a certificate that
//   does not verify (CERT_HAS_EXPIRED), ERR_TLS_* and ERR_SSL_*, and the
//   ECONNRESET of a handshake the server ended.
// - Result, ok and err (docs/errors.md, section 1): Ok and Err print as tz
//   prints them, except that tz adds where an Err was made.
// - JSON.parse, decodeURIComponent, decodeURI, URL.parse and Date.parse,
//   which give a Result in tz, and RegExp.parse, Intl.Locale.parse and
//   TextDecoder.parse, which only tz has; decode of a TextDecoder with fatal,
//   which gives a Result too.
// - "node:fs" and "node:fs/promises" whose functions give a Result
//   (tz-fs.mjs, tz-fs-promises.mjs), and "node:child_process" whose
//   execSync, execFileSync and spawnSync give one (tz-child-process.mjs), and
//   "node:crypto" with a Result where the data may be wrong (tz-crypto.mjs),
//   "node:readline/promises" whose question() gives one
//   (tz-readline-promises.mjs), "node:events" whose once() and on() give one
//   (tz-events.mjs), and "node:stream/consumers" and
//   "node:stream/promises" (tz-stream-consumers.mjs, tz-stream-promises.mjs).
// - end() of writable streams and `for await` over readable ones, which give
//   Results in tz.
// - The conversions of numbers (docs/numbers.md, section 3): x.toInt(),
//   x.toInt32(), x.toDouble() and the others give the same number, as in tz
//   when the value fits, which the tests compared with node keep to.
// - parseInt, parseFloat and Number of a string give a Result (docs/numbers.md,
//   section 9): an Err where JavaScript gives NaN, an infinity, or for
//   parseInt a number beyond 64 bits. Only for the test's own code: node's
//   own code, and the files next to this one, still get plain numbers.
// - charCodeAt past the end gives undefined, where JavaScript gives NaN, also
//   only for the test's own code.
// - Doubles read from bytes (getFloat64, readDoubleLE and the others) give a
//   Result, and Math.min() and Math.max() of an empty spread give undefined
//   (docs/numbers.md, section 10); Math.clamp, which node does not have yet.
// - The module "tz:io", with readLine (tz-io.mjs next to this file).
// - The module "tz:http", with Server over node:http (tz-http.mjs).
// - NODE_OPTIONS with this file, for the tests that run themselves again as
//   a child process.
"use strict";

const { registerHooks } = require("node:module");
const { join } = require("node:path");
const { pathToFileURL } = require("node:url");

// The codes node gives a certificate that does not verify (X509ErrorCode).
const certificateCodes = new Set([
  "UNABLE_TO_GET_ISSUER_CERT", "UNABLE_TO_GET_CRL", "UNABLE_TO_DECRYPT_CERT_SIGNATURE",
  "UNABLE_TO_DECRYPT_CRL_SIGNATURE", "UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY",
  "CERT_SIGNATURE_FAILURE", "CRL_SIGNATURE_FAILURE", "CERT_NOT_YET_VALID", "CERT_HAS_EXPIRED",
  "CRL_NOT_YET_VALID", "CRL_HAS_EXPIRED", "ERROR_IN_CERT_NOT_BEFORE_FIELD",
  "ERROR_IN_CERT_NOT_AFTER_FIELD", "ERROR_IN_CRL_LAST_UPDATE_FIELD",
  "ERROR_IN_CRL_NEXT_UPDATE_FIELD", "OUT_OF_MEM", "DEPTH_ZERO_SELF_SIGNED_CERT",
  "SELF_SIGNED_CERT_IN_CHAIN", "UNABLE_TO_GET_ISSUER_CERT_LOCALLY",
  "UNABLE_TO_VERIFY_LEAF_SIGNATURE", "CERT_CHAIN_TOO_LONG", "CERT_REVOKED", "INVALID_CA",
  "PATH_LENGTH_EXCEEDED", "INVALID_PURPOSE", "CERT_UNTRUSTED", "CERT_REJECTED",
  "HOSTNAME_MISMATCH", "UNSPECIFIED",
]);

function isTlsError(e) {
  return certificateCodes.has(e.code) || e.code.startsWith("ERR_TLS_") ||
         e.code.startsWith("ERR_SSL_") ||
         (e.code === "ECONNRESET" && e.message.startsWith("Client network socket disconnected"));
}

globalThis.SystemError = class SystemError extends Error {
  static [Symbol.hasInstance](value) {
    return value instanceof Error && typeof value.code === "string" &&
           (typeof value.syscall === "string" || isTlsError(value));
  }
};

class Ok {
  constructor(value) { this.value = value; }
  isOk() { return true; }
  isErr() { return false; }
  map(f) { return new Ok(f(this.value)); }
  mapErr() { return this; }
  andThen(f) { return f(this.value); }
  orElse() { return this; }
  match(onOk) { return onOk(this.value); }
  unwrapOr() { return this.value; }
  andTee(f) { f(this.value); return this; }
  orTee() { return this; }
  andThrough(f) { const r = f(this.value); return r.isErr() ? r : this; }
}

class Err {
  constructor(error) { this.error = error; }
  isOk() { return false; }
  isErr() { return true; }
  map() { return this; }
  mapErr(f) { return new Err(f(this.error)); }
  andThen() { return this; }
  orElse(f) { return f(this.error); }
  match(onOk, onErr) { return onErr(this.error); }
  unwrapOr(fallback) { return fallback; }
  andTee() { return this; }
  orTee(f) { f(this.error); return this; }
  andThrough() { return this; }
}

globalThis.ok = (value) => new Ok(value);

const testsNode = __dirname.toLowerCase();
function fromNode() {
  const saved = Error.prepareStackTrace;
  Error.prepareStackTrace = (_, frames) => frames;
  const holder = {};
  Error.captureStackTrace(holder, fromNode);
  const frames = holder.stack;
  Error.prepareStackTrace = saved;
  const caller = (Array.isArray(frames) && frames[1] && frames[1].getFileName()) || "";
  if (caller.startsWith("node:")) return true;
  const path = caller.startsWith("file:") ? require("node:url").fileURLToPath(caller) : caller;
  return path.toLowerCase().startsWith(testsNode);
}

{
  const finite = (value) =>
    Number.isFinite(value)
      ? new Ok(value)
      : new Err({
          kind: "NUMBER",
          code: "ERR_INVALID_NUMBER",
          message: value !== value ? "the bytes hold NaN, which a tz double cannot be"
                                   : "the bytes hold an infinity, which a tz double cannot be",
        });
  const wrap = (proto, names) => {
    for (const name of names) {
      const plain = proto[name];
      if (typeof plain !== "function") continue;
      Object.defineProperty(proto, name, {
        value(...args) {
          const value = plain.apply(this, args);
          return fromNode() ? value : finite(value);
        },
        configurable: true,
        writable: true,
      });
    }
  };
  wrap(DataView.prototype, ["getFloat16", "getFloat32", "getFloat64"]);
  wrap(require("node:buffer").Buffer.prototype,
       ["readFloatLE", "readFloatBE", "readDoubleLE", "readDoubleBE"]);
  Math.clamp = (x, lo, hi) => Math.min(Math.max(x, lo), hi);
  for (const name of ["min", "max"]) {
    const plain = Math[name];
    Math[name] = function (...args) {
      return args.length === 0 && !fromNode() ? undefined : plain.apply(Math, args);
    };
  }
}

{
  const plainCharCodeAt = String.prototype.charCodeAt;
  Object.defineProperty(String.prototype, "charCodeAt", {
    value(index) {
      const code = plainCharCodeAt.call(this, index);
      return code === code || fromNode() ? code : undefined;
    },
    configurable: true,
    writable: true,
  });
}

{
  const plainNumber = Number;
  const plainParseInt = parseInt;
  const plainParseFloat = parseFloat;
  const numberResult = (value, text, integer) => {
    if (Number.isNaN(value)) return new Err({ kind: "NUMBER", code: "ERR_INVALID_NUMBER", message: `not a number: '${text}'` });
    if (!Number.isFinite(value) || (integer && Math.abs(value) >= 2 ** 63)) {
      return new Err({ kind: "NUMBER", code: "ERR_INVALID_NUMBER", message: `the number is out of range: '${text}'` });
    }
    return new Ok(value);
  };
  function parsingInt(text, radix) {
    if (fromNode()) return plainParseInt(text, radix);
    return numberResult(plainParseInt(text, radix), text, true);
  }
  function parsingFloat(text) {
    if (fromNode()) return plainParseFloat(text);
    return numberResult(plainParseFloat(text), text, false);
  }
  function TzNumber(...args) {
    if (args.length > 0 && typeof args[0] === "string" && !fromNode()) {
      return numberResult(plainNumber(args[0]), args[0], false);
    }
    return plainNumber(...args);
  }
  for (const key of Object.getOwnPropertyNames(plainNumber)) {
    if (!(key in TzNumber)) TzNumber[key] = plainNumber[key];
  }
  TzNumber.parseInt = parsingInt;
  TzNumber.parseFloat = parsingFloat;
  globalThis.Number = TzNumber;
  globalThis.parseInt = parsingInt;
  globalThis.parseFloat = parsingFloat;
}

for (const name of ["toInt", "toInt8", "toInt16", "toInt32", "toUint8", "toUint16", "toUint32",
                    "toUint64", "toDouble"]) {
  Object.defineProperty(Object.getPrototypeOf(0), name, {
    value() { return Number(this); },
    configurable: true,
  });
}

// JSON.stringify of a value whose type may hold itself (Json, a tree of
// classes) gives a Result in tz (docs/errors.md, section 7); a string here
// answers as an Ok, so a test reads it both ways with unwrapOr and isOk.
Object.defineProperties(String.prototype, {
  isOk: { value() { return true; }, configurable: true },
  isErr: { value() { return false; }, configurable: true },
  unwrapOr: { value() { return String(this); }, configurable: true },
});

// Reflect.apply and Reflect.construct give a Result in tz (docs/decorators.md,
// section 6); the tests compared with node call them only where they work.
const reflectApply = Reflect.apply;
const reflectConstruct = Reflect.construct;
Reflect.apply = (f, self, args) => new Ok(reflectApply(f, self, args));
Reflect.construct = (cls, args) => new Ok(reflectConstruct(cls, args));
globalThis.err = (error) => new Err(error);
globalThis.Result = {
  combine(results) {
    const values = [];
    for (const r of results) {
      if (r.isErr()) return r;
      values.push(r.value);
    }
    return new Ok(values);
  },
  combineWithAllErrors(results) {
    const errors = results.filter((r) => r.isErr()).map((r) => r.error);
    return errors.length ? new Err(errors) : new Ok(results.map((r) => r.value));
  },
};

const parseJson = JSON.parse;
JSON.parse = (text) => {
  try {
    return new Ok(parseJson(text));
  } catch (e) {
    return new Err({ kind: "JSON", code: "ERR_INVALID_JSON", message: e.message });
  }
};

function resulting(f, kind, code) {
  return (text) => {
    try {
      return new Ok(f(text));
    } catch (e) {
      return new Err({ kind, code, message: e.message });
    }
  };
}
globalThis.decodeURIComponent = resulting(decodeURIComponent, "URI", "ERR_INVALID_URI");
globalThis.decodeURI = resulting(decodeURI, "URI", "ERR_INVALID_URI");
const parseDate = Date.parse;
Date.parse = (text) => {
  const ms = parseDate(text);
  return Number.isNaN(ms) ? new Err({ kind: "DATE", code: "ERR_INVALID_DATE", message: "Invalid Date", text }) : new Ok(ms);
};
const parseUrl = URL.parse;
URL.parse = (input, base) => {
  const url = parseUrl(input, base);
  return url ? new Ok(url) : new Err({ kind: "URL", code: "ERR_INVALID_URL", message: "Invalid URL", input });
};
RegExp.parse = (pattern, flags) => {
  try {
    return new Ok(new RegExp(pattern, flags));
  } catch (e) {
    return new Err({ kind: "REGEX", code: "ERR_INVALID_REGEX", message: e.message });
  }
};
globalThis.TextDecoder = class TextDecoder extends globalThis.TextDecoder {
  decode(input, options) {
    if (!this.fatal) return super.decode(input, options);
    try {
      return new Ok(super.decode(input, options));
    } catch (e) {
      return new Err({ code: e.code, message: e.message });
    }
  }
  static parse(label, options) {
    try {
      return new Ok(new TextDecoder(label, options));
    } catch (e) {
      return new Err({ code: e.code, message: e.message });
    }
  }
};
Intl.Locale.parse = (tag) => {
  try {
    return new Ok(new Intl.Locale(tag));
  } catch (e) {
    return new Err({ kind: "LOCALE", code: "ERR_INVALID_LOCALE", message: e.message, tag });
  }
};

// Streams in tz (docs/streams.md, section 3): end() of a writable gives a
// Promise of a Result, which says when the stream finished and whether it
// failed, and `for await` over a readable gives each chunk in a Result, an
// Err at the end when the stream fails. Node's own code iterates readables
// too, so only an iteration the test itself starts gets Results.
{
  const stream = require("node:stream");
  for (const proto of [stream.Writable.prototype, stream.Duplex.prototype]) {
    const end = proto.end;
    proto.end = function (...args) {
      return new Promise((resolve) => {
        end.call(this, ...args, (e) => resolve(e ? new Err(e) : new Ok(undefined)));
      });
    };
  }
  const iterate = stream.Readable.prototype[Symbol.asyncIterator];
  const iterator = stream.Readable.prototype.iterator;
  async function* results(chunks) {
    try {
      for await (const chunk of chunks) yield new Ok(chunk);
    } catch (e) {
      yield new Err(e);
    }
  }
  const fromTest = () => !(new Error().stack.split("\n")[3] ?? "").includes("node:");
  stream.Readable.prototype[Symbol.asyncIterator] = function () {
    const chunks = iterate.call(this);
    return fromTest() ? results(chunks) : chunks;
  };
  stream.Readable.prototype.iterator = function (options) {
    const chunks = iterator.call(this, options);
    return fromTest() ? results(chunks) : chunks;
  };
  // The same for a web ReadableStream (the body of fetch): each chunk in a
  // Result, and so is what read() of its reader gives.
  const web = globalThis.ReadableStream.prototype;
  const values = web.values;
  web[Symbol.asyncIterator] = web.values = function (options) {
    const chunks = values.call(this, options);
    return fromTest() ? results(chunks) : chunks;
  };
  const readerProto = Object.getPrototypeOf(new globalThis.ReadableStream().getReader());
  const read = readerProto.read;
  readerProto.read = async function () {
    if (!fromTest()) return read.call(this);
    try {
      return new Ok(await read.call(this));
    } catch (e) {
      return new Err(e);
    }
  };
}

// crypto.subtle gives a Promise of a Result in tz, with a code of tz for the
// DOMException (or TypeError) node rejects with.
const subtleCodes = {
  NotSupportedError: "ERR_NOT_SUPPORTED", DataError: "ERR_INVALID_DATA",
  OperationError: "ERR_OPERATION_FAILED", InvalidAccessError: "ERR_INVALID_ACCESS",
  InvalidAccessException: "ERR_INVALID_ACCESS", SyntaxError: "ERR_INVALID_KEY_USAGE",
};
const subtleProto = Object.getPrototypeOf(globalThis.crypto.subtle);
for (const name of ["digest", "generateKey", "importKey", "exportKey", "sign", "verify", "encrypt",
                    "decrypt", "deriveBits", "deriveKey", "wrapKey", "unwrapKey"]) {
  const original = subtleProto[name];
  subtleProto[name] = async function (...args) {
    try {
      return new Ok(await original.apply(this, args));
    } catch (e) {
      const code = e instanceof DOMException
        ? subtleCodes[e.name] ?? e.name
        : e instanceof RangeError ? "ERR_OUT_OF_RANGE" : "ERR_INVALID_ARG_TYPE";
      return new Err({ kind: "SUBTLE", code, message: e.message });
    }
  };
}

// fetch and the bodies of Request and Response give a Result in tz
// (docs/net.md, section 2), with errors of a kind and a code and tz's texts,
// built here from what node throws; the timeout of a fetch is the signal of
// AbortSignal.timeout() in node.
const tlsCodes = new Set([
  "UNABLE_TO_GET_ISSUER_CERT", "UNABLE_TO_GET_CRL", "UNABLE_TO_DECRYPT_CERT_SIGNATURE",
  "UNABLE_TO_DECRYPT_CRL_SIGNATURE", "UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY",
  "CERT_SIGNATURE_FAILURE", "CRL_SIGNATURE_FAILURE", "CERT_NOT_YET_VALID", "CERT_HAS_EXPIRED",
  "CRL_NOT_YET_VALID", "CRL_HAS_EXPIRED", "ERROR_IN_CERT_NOT_BEFORE_FIELD",
  "ERROR_IN_CERT_NOT_AFTER_FIELD", "ERROR_IN_CRL_LAST_UPDATE_FIELD",
  "ERROR_IN_CRL_NEXT_UPDATE_FIELD", "OUT_OF_MEM", "DEPTH_ZERO_SELF_SIGNED_CERT",
  "SELF_SIGNED_CERT_IN_CHAIN", "UNABLE_TO_GET_ISSUER_CERT_LOCALLY",
  "UNABLE_TO_VERIFY_LEAF_SIGNATURE", "CERT_CHAIN_TOO_LONG", "CERT_REVOKED", "INVALID_CA",
  "PATH_LENGTH_EXCEEDED", "INVALID_PURPOSE", "CERT_UNTRUSTED", "CERT_REJECTED",
  "HOSTNAME_MISMATCH", "UNSPECIFIED", "ERR_TLS_CERT_ALTNAME_INVALID",
]);
const networkCodes = new Set([
  "ENOTFOUND", "EAI_AGAIN", "ECONNREFUSED", "ETIMEDOUT", "ENETUNREACH", "EHOSTUNREACH",
  "EADDRNOTAVAIL", "ECONNRESET", "ECONNABORTED", "EPIPE", "EMFILE", "ENOBUFS",
]);
function peerOf(input) {
  const url = new URL(typeof input === "string" ? input : input.url ?? String(input));
  const host = url.hostname.startsWith("[") ? url.hostname.slice(1, -1) : url.hostname;
  const port = url.port === "" ? (url.protocol === "https:" ? 443 : 80) : Number(url.port);
  return { url, host, port, name: `${host}:${port}` };
}
function tlsMessage(code, host) {
  const hint = process.env.NODE_USE_SYSTEM_CA === "1" ? ""
    : "; if its root CA is installed on this machine, run with NODE_USE_SYSTEM_CA=1";
  switch (code) {
    case "CERT_HAS_EXPIRED": return `the certificate of ${host} has expired`;
    case "CERT_NOT_YET_VALID": return `the certificate of ${host} is not valid yet`;
    case "DEPTH_ZERO_SELF_SIGNED_CERT":
      return `the certificate of ${host} is self-signed, so it is not trusted${hint}`;
    case "SELF_SIGNED_CERT_IN_CHAIN":
      return `the certificates of ${host} lead to a root CA that is not trusted`;
    case "UNABLE_TO_VERIFY_LEAF_SIGNATURE":
      return `the certificate of ${host} is signed by an authority that is not known${hint}`;
    case "UNABLE_TO_GET_ISSUER_CERT_LOCALLY":
      return `the certificate of ${host} is signed by an authority that is not known`;
    case "ERR_TLS_CERT_ALTNAME_INVALID":
    case "HOSTNAME_MISMATCH":
      return `the certificate is not for ${host}`;
    case "CERT_REVOKED": return `the certificate of ${host} is revoked`;
    default: return `the certificate of ${host} is not trusted`;
  }
}
function networkMessage(code, peer, stage, detail) {
  const name = peer.name;
  switch (code) {
    case "ENOTFOUND": return `could not find the address of ${peer.host}: no such name in DNS`;
    case "EAI_AGAIN":
      return `could not find the address of ${peer.host}: DNS did not answer; check the network`;
    case "ECONNREFUSED": return `could not connect to ${name}: nothing listens on this port`;
    case "ETIMEDOUT": return `could not connect to ${name}: the host did not answer in time`;
    case "ENETUNREACH": return `could not connect to ${name}: the network is unreachable`;
    case "EHOSTUNREACH": return `could not connect to ${name}: the host is unreachable`;
    case "EADDRNOTAVAIL":
      return `could not connect to ${name}: the address is not available on this machine`;
    case "ECONNRESET": return `the connection to ${name} was reset while ${stage}`;
    case "ECONNABORTED":
      return `the connection to ${name} was aborted on this machine while ${stage}`;
    case "EPIPE": return `the connection to ${name} broke while ${stage}`;
    case "EMFILE":
      return `could not connect to ${name}: too many files and sockets are open in this program`;
    case "ENOBUFS": return `could not connect to ${name}: the system has no buffer space left`;
    default: return `the connection to ${name} failed while ${stage}: ${detail}`;
  }
}
function requestError(code, message, url) {
  return { kind: "REQUEST", code, message, url };
}
function fetchError(e, input, init) {
  const given = typeof input === "string" ? input : input instanceof URL ? input.href : input.url;
  const message = e.message ?? "";
  if (e instanceof DOMException) {
    const peer = peerOf(input);
    return { kind: "TIMEOUT", code: "TIMEOUT_ERR",
             message: `no full answer from ${peer.host} within ${init.timeout} ms`,
             timeout: init.timeout };
  }
  if (message.startsWith("Failed to parse URL from"))
    return requestError("ERR_INVALID_URL", `fetch cannot parse "${given}" as a URL`, given);
  if (message.includes("includes credentials"))
    return requestError("ERR_CREDENTIALS_IN_URL",
                        `the URL ${given} has a user name or a password; send them in the ` +
                        "authorization header instead", given);
  const method = init?.method ?? "GET";
  if (message.includes("is not a valid HTTP method"))
    return requestError("ERR_INVALID_METHOD", `'${method}' is not a valid HTTP method`, given);
  if (message.includes("HTTP method is unsupported"))
    return requestError("ERR_INVALID_METHOD", `fetch does not send ${method.toUpperCase()} requests`,
                        given);
  if (message.includes("cannot have body"))
    return requestError("ERR_BODY_WITH_GET",
                        `a ${method.toUpperCase()} request cannot have a body: use POST, or drop ` +
                        "the body", given);
  if (message.includes("duplex option is required"))
    return requestError("ERR_DUPLEX_REQUIRED",
                        'a request whose body is a stream needs duplex: "half"', given);
  if (message.includes("has already been used"))
    return requestError("ERR_REQUEST_USED",
                        "the body of this Request is already read, so it cannot be sent", given);
  const peer = peerOf(input);
  const cause = e.cause ?? {};
  const why = cause.message ?? "";
  if (why === "bad port")
    return requestError("ERR_BLOCKED_PORT",
                        `fetch does not connect to port ${peer.port}: browsers and node block it too`,
                        peer.url.href);
  if (why.includes("not implemented") || why.includes("unknown scheme"))
    return requestError("ERR_UNSUPPORTED_SCHEME",
                        `fetch cannot get ${peer.url.href}: only http:, https: and data: work`,
                        peer.url.href);
  if (why === "unexpected redirect")
    return { kind: "REDIRECT", code: "ERR_UNEXPECTED_REDIRECT",
             message: `${peer.url.href} redirects, and redirect is "error"`, location: "" };
  if (why === "redirect count exceeded")
    return { kind: "REDIRECT", code: "ERR_TOO_MANY_REDIRECTS", message: "more than 20 redirects",
             location: "" };
  if (tlsCodes.has(cause.code))
    return { kind: "TLS", code: cause.code, message: tlsMessage(cause.code, peer.host),
             host: peer.host };
  if (cause.code === "UND_ERR_SOCKET")
    return { kind: "RESPONSE", code: "ERR_CLOSED",
             message: `${peer.name} closed the connection before the response`, host: peer.host,
             port: peer.port };
  if (typeof cause.code === "string" && cause.code.startsWith("HPE_"))
    return { kind: "RESPONSE", code: "ERR_INVALID_RESPONSE",
             message: `${peer.name} did not answer in HTTP/1.1: ${why}`, host: peer.host,
             port: peer.port };
  const code = networkCodes.has(cause.code) ? cause.code : "UNKNOWN";
  return { kind: "NETWORK", code, message: networkMessage(code, peer, "connecting", why),
           errno: cause.errno ?? -1, syscall: cause.syscall ?? "", host: peer.host,
           port: peer.port };
}
const originalFetch = globalThis.fetch;
globalThis.fetch = async (input, init) => {
  const timeout = init?.timeout;
  const given = timeout === undefined ? init : { ...init, signal: AbortSignal.timeout(timeout) };
  try {
    const response = await originalFetch(input, given);
    responsePeers.set(response, { peer: peerOf(input), timeout });
    return new Ok(response);
  } catch (e) {
    return new Err(fetchError(e, input, init));
  }
};
const responsePeers = new WeakMap();
function bodyError(e, owner) {
  const known = responsePeers.get(owner);
  if (e instanceof DOMException) {
    const timeout = known?.timeout ?? 0;
    return { kind: "TIMEOUT", code: "TIMEOUT_ERR",
             message: `no full answer from ${known?.peer.host} within ${timeout} ms`, timeout };
  }
  const message = e.message;
  if (message.startsWith("Body is unusable"))
    return { kind: "USED", code: "ERR_BODY_USED",
             message: "the body is already read: text(), json(), bytes(), formData() and body() " +
                      "read it once" };
  if (message.includes("locked"))
    return { kind: "USED", code: "ERR_BODY_LOCKED",
             message: "the body is locked: a reader of body() holds it" };
  if (message === "terminated") {
    const cause = e.cause;
    if (cause?.code?.startsWith("Z_") || cause?.code?.startsWith("ERR_BROTLI") ||
        cause?.errno !== undefined && cause?.code === undefined) {
      const coding = owner.headers.get("content-encoding") ?? "";
      return { kind: "MALFORMED", code: "ERR_INVALID_COMPRESSION",
               message: `the body is not valid ${coding}: ${cause.message}` };
    }
    const who = known === undefined ? "the client" : known.peer.name;
    return { kind: "NETWORK", code: "ERR_CLOSED",
             message: `${who} closed the connection before the end of the body` };
  }
  return { kind: "STREAM", code: "ERR_STREAM_FAILED",
           message: `the stream of the body failed: ${message}` };
}
function formError(owner) {
  const header = owner.headers.get("content-type") ?? undefined;
  const given = header ?? "";
  const type = given.toLowerCase();
  const withType = (error) => (header === undefined ? error : { ...error, contentType: header });
  if (type.startsWith("multipart/form-data") && !/boundary=/i.test(given))
    return withType({ kind: "CONTENT", code: "ERR_NO_BOUNDARY",
                      message: "the body is not a form: multipart/form-data has no boundary" });
  if (type.startsWith("multipart/form-data"))
    return withType({ kind: "CONTENT", code: "ERR_INVALID_MULTIPART",
                      message: "the body is not a valid multipart form: a part has no end of its head" });
  const what = given === "" ? "it has no content-type" : `content-type is ${given}`;
  return withType({ kind: "CONTENT", code: "ERR_NOT_A_FORM", message: `the body is not a form: ${what}` });
}
for (const proto of [Response.prototype, Request.prototype]) {
  for (const name of ["text", "bytes", "arrayBuffer", "formData"]) {
    const original = proto[name];
    proto[name] = async function () {
      try {
        return new Ok(await original.call(this));
      } catch (e) {
        if (name === "formData" && e.message.startsWith("Failed to parse body as FormData"))
          return new Err(formError(this));
        return new Err(bodyError(e, this));
      }
    };
  }
  proto.json = async function () {
    const text = await this.text();
    if (text.isErr()) return text;
    const parsed = JSON.parse(text.value);
    if (parsed.isOk()) return parsed;
    const header = this.headers.get("content-type") ?? undefined;
    const error = { kind: "CONTENT", code: "ERR_INVALID_JSON",
                    message: `the body is not JSON: ${parsed.error.message}` };
    if (header !== undefined) error.contentType = header;
    error.start = text.value.slice(0, 200);
    return new Err(error);
  };
}
// A value that the object changes itself is a method in tz (docs/objects.md,
// section 6): for test code the getter gives a function, for node's own code
// the value. A setter stays as it was, and a field that node's own code
// assigns becomes a getter with a setter that keeps its value.
function asMethod(proto, name) {
  const own = Object.getOwnPropertyDescriptor(proto, name);
  if (own !== undefined && own.get !== undefined) {
    const get = own.get;
    Object.defineProperty(proto, name, {
      configurable: true,
      get() {
        return fromNode() ? get.call(this) : () => get.call(this);
      },
      set: own.set,
    });
    return;
  }
  const slot = Symbol(name);
  Object.defineProperty(proto, name, {
    configurable: true,
    get() {
      const value = this[slot];
      return fromNode() ? value : () => value;
    },
    set(value) {
      this[slot] = value;
    },
  });
}
asMethod(Response.prototype, "bodyUsed");
asMethod(Request.prototype, "bodyUsed");
asMethod(Response.prototype, "body");
asMethod(Request.prototype, "body");
asMethod(ReadableStreamDefaultController.prototype, "desiredSize");
asMethod(globalThis.ReadableStream.prototype, "locked");
for (const name of ["href", "origin", "protocol", "username", "password", "host", "hostname",
                    "port", "pathname", "search", "hash"])
  asMethod(URL.prototype, name);
asMethod(URLSearchParams.prototype, "size");
URL.prototype.portNumber = function () {
  return this.port === "" ? undefined : Number(this.port);
};
for (const name of ["target", "currentTarget", "srcElement", "eventPhase", "defaultPrevented"])
  asMethod(Event.prototype, name);
// The parts of a URL change through its methods in tz; the ones that can
// refuse a value give a Result.
for (const name of ["pathname", "search", "hash", "username", "password"]) {
  URL.prototype["set" + name[0].toUpperCase() + name.slice(1)] = function (value) {
    this[name] = value;
  };
}
for (const name of ["href", "protocol", "host", "hostname", "port"]) {
  URL.prototype["set" + name[0].toUpperCase() + name.slice(1)] = function (value) {
    const before = this.href;
    try {
      this[name] = value;
    } catch (e) {
      return new Err({ kind: "URL", code: "ERR_INVALID_URL", message: "Invalid URL", input: value });
    }
    const wanted = name === "protocol" ? value.replace(/:$/, "") + ":" : value;
    if (this.href === before && this[name] !== wanted)
      return new Err({ kind: "URL", code: "ERR_INVALID_URL", message: "Invalid " + name, input: value });
    return new Ok(undefined);
  };
}

const tzIo = pathToFileURL(join(__dirname, "tz-io.mjs")).href;
const tzHttp = pathToFileURL(join(__dirname, "tz-http.mjs")).href;
const tzFs = pathToFileURL(join(__dirname, "tz-fs.mjs")).href;
const tzFsPromises = pathToFileURL(join(__dirname, "tz-fs-promises.mjs")).href;
const tzChildProcess = pathToFileURL(join(__dirname, "tz-child-process.mjs")).href;
const tzCrypto = pathToFileURL(join(__dirname, "tz-crypto.mjs")).href;
const tzReadline = pathToFileURL(join(__dirname, "tz-readline-promises.mjs")).href;
const tzEvents = pathToFileURL(join(__dirname, "tz-events.mjs")).href;
const tzConsumers = pathToFileURL(join(__dirname, "tz-stream-consumers.mjs")).href;
const tzStreamPromises = pathToFileURL(join(__dirname, "tz-stream-promises.mjs")).href;
const here = pathToFileURL(__dirname).href + "/";
// The state of a node stream, and isTrusted of an event, are properties in
// node that cannot be redefined (non-configurable, or own properties of each
// object), so the test's own code reads them as fields there.
const fieldsInNode = new RegExp(
  String.raw`\.(destroyed|closed|errored|readable|readableAborted|readableDidRead|` +
    String.raw`readableEncoding|readableEnded|readableFlowing|readableHighWaterMark|` +
    String.raw`readableLength|readableObjectMode|writable|writableAborted|writableEnded|` +
    String.raw`writableFinished|writableHighWaterMark|writableLength|writableObjectMode|` +
    String.raw`writableCorked|writableNeedDrain|allowHalfOpen|bytesRead|bytesWritten|pending|` +
    String.raw`isTrusted)\(\)`,
  "g");
function testCode(url) {
  return url.startsWith("file:") && !url.startsWith(here) && !url.includes("/node_modules/");
}
registerHooks({
  load(url, context, nextLoad) {
    const loaded = nextLoad(url, context);
    if (!testCode(url) || loaded.source == null) return loaded;
    const source = String(loaded.source).replace(fieldsInNode, ".$1");
    return { ...loaded, source };
  },
  resolve(specifier, context, nextResolve) {
    const own = context.parentURL?.startsWith(here) ?? false;
    if (!own && (specifier === "node:fs" || specifier === "fs"))
      return { url: tzFs, shortCircuit: true };
    if (!own && (specifier === "node:fs/promises" || specifier === "fs/promises"))
      return { url: tzFsPromises, shortCircuit: true };
    if (!own && (specifier === "node:child_process" || specifier === "child_process"))
      return { url: tzChildProcess, shortCircuit: true };
    if (!own && (specifier === "node:crypto" || specifier === "crypto"))
      return { url: tzCrypto, shortCircuit: true };
    if (!own && (specifier === "node:readline/promises" || specifier === "readline/promises"))
      return { url: tzReadline, shortCircuit: true };
    if (!own && (specifier === "node:events" || specifier === "events"))
      return { url: tzEvents, shortCircuit: true };
    if (!own && (specifier === "node:stream/consumers" || specifier === "stream/consumers"))
      return { url: tzConsumers, shortCircuit: true };
    if (!own && (specifier === "node:stream/promises" || specifier === "stream/promises"))
      return { url: tzStreamPromises, shortCircuit: true };
    if (specifier === "tz:io")
      return { url: tzIo, shortCircuit: true };
    if (specifier === "tz:http")
      return { url: tzHttp, shortCircuit: true };
    return nextResolve(specifier, context);
  },
});

const preload = `--require ${JSON.stringify(__filename)}`;
if (!(process.env.NODE_OPTIONS ?? "").includes(preload))
  process.env.NODE_OPTIONS = `${process.env.NODE_OPTIONS ?? ""} ${preload}`.trim();
