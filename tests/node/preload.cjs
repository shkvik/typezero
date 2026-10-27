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
//   "node:timers/promises" whose waits with a signal give one
//   (tz-timers-promises.mjs), "node:readline/promises" whose question()
//   gives one (tz-readline-promises.mjs), "node:events" whose once() and
//   on() give one (tz-events.mjs), and "node:stream/consumers" and
//   "node:stream/promises" (tz-stream-consumers.mjs, tz-stream-promises.mjs).
// - end() of writable streams and `for await` over readable ones, which give
//   Results in tz.
// - The module "tz:io", with readLine (tz-io.mjs next to this file).
// - The module "tz:http", with serve over node:http (tz-http.mjs).
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
    return new Err({ code: "bad_json", message: e.message, path: "" });
  }
};

function resulting(f, code) {
  return (text) => {
    try {
      return new Ok(f(text));
    } catch (e) {
      return new Err({ code, message: e.message });
    }
  };
}
globalThis.decodeURIComponent = resulting(decodeURIComponent, "bad_uri");
globalThis.decodeURI = resulting(decodeURI, "bad_uri");
const parseDate = Date.parse;
Date.parse = (text) => {
  const ms = parseDate(text);
  return Number.isNaN(ms) ? new Err({ code: "bad_date", message: "Invalid Date", text }) : new Ok(ms);
};
const parseUrl = URL.parse;
URL.parse = (input, base) => {
  const url = parseUrl(input, base);
  return url ? new Ok(url) : new Err({ code: "bad_url", message: "Invalid URL", input });
};
RegExp.parse = (pattern, flags) => {
  try {
    return new Ok(new RegExp(pattern, flags));
  } catch (e) {
    return new Err({ code: "bad_regex", message: e.message });
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
    return new Err({ code: "bad_locale", message: e.message, tag });
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

// crypto.subtle gives a Promise of a Result in tz, with the name of the
// DOMException (or TypeError) node rejects with as the code.
const subtleProto = Object.getPrototypeOf(globalThis.crypto.subtle);
for (const name of ["digest", "generateKey", "importKey", "exportKey", "sign", "verify", "encrypt",
                    "decrypt", "deriveBits", "deriveKey", "wrapKey", "unwrapKey"]) {
  const original = subtleProto[name];
  subtleProto[name] = async function (...args) {
    try {
      return new Ok(await original.apply(this, args));
    } catch (e) {
      const code = e instanceof DOMException
        ? (e.name === "InvalidAccessException" ? "InvalidAccessError" : e.name)
        : e instanceof RangeError ? "RangeError" : "TypeError";
      return new Err({ code, message: e.message });
    }
  };
}

// fetch and the bodies of Request and Response give a Result in tz
// (docs/net.md, section 2).
function abortCode(e) {
  return e.name === "TimeoutError" ? "TIMEOUT_ERR" : "ABORT_ERR";
}
const originalFetch = globalThis.fetch;
globalThis.fetch = async (...args) => {
  try {
    return new Ok(await originalFetch(...args));
  } catch (e) {
    if (e instanceof DOMException) return new Err({ code: abortCode(e), message: e.message });
    const cause = e.cause;
    if (e.message !== "fetch failed")
      return new Err(cause === undefined ? { code: "bad_request", message: e.message }
                                         : { code: "bad_request", message: e.message, cause: cause.message });
    if (cause instanceof SystemError)
      return new Err({ code: cause.code, message: e.message, cause: cause.message });
    if (cause !== undefined) return new Err({ code: "fetch_failed", message: e.message, cause: cause.message });
    return new Err({ code: "fetch_failed", message: e.message });
  }
};
function bodyError(e) {
  if (e instanceof DOMException) return { code: abortCode(e), message: e.message };
  const message = e.message;
  if (message.startsWith("Body is unusable")) return { code: "body_used", message };
  if (message === "terminated")
    return e.cause === undefined ? { code: "terminated", message } : { code: "terminated", message, cause: e.cause.message };
  if (message.startsWith("Failed to parse body as FormData")) return { code: "bad_form", message };
  return { code: "body_failed", message };
}
for (const proto of [Response.prototype, Request.prototype]) {
  for (const name of ["text", "bytes", "arrayBuffer", "formData"]) {
    const original = proto[name];
    proto[name] = async function () {
      try {
        return new Ok(await original.call(this));
      } catch (e) {
        return new Err(bodyError(e));
      }
    };
  }
  proto.json = async function () {
    const text = await this.text();
    return text.isErr() ? text : JSON.parse(text.value);
  };
}

const tzIo = pathToFileURL(join(__dirname, "tz-io.mjs")).href;
const tzHttp = pathToFileURL(join(__dirname, "tz-http.mjs")).href;
const tzFs = pathToFileURL(join(__dirname, "tz-fs.mjs")).href;
const tzFsPromises = pathToFileURL(join(__dirname, "tz-fs-promises.mjs")).href;
const tzChildProcess = pathToFileURL(join(__dirname, "tz-child-process.mjs")).href;
const tzCrypto = pathToFileURL(join(__dirname, "tz-crypto.mjs")).href;
const tzTimers = pathToFileURL(join(__dirname, "tz-timers-promises.mjs")).href;
const tzReadline = pathToFileURL(join(__dirname, "tz-readline-promises.mjs")).href;
const tzEvents = pathToFileURL(join(__dirname, "tz-events.mjs")).href;
const tzConsumers = pathToFileURL(join(__dirname, "tz-stream-consumers.mjs")).href;
const tzStreamPromises = pathToFileURL(join(__dirname, "tz-stream-promises.mjs")).href;
const here = pathToFileURL(__dirname).href + "/";
registerHooks({
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
    if (!own && (specifier === "node:timers/promises" || specifier === "timers/promises"))
      return { url: tzTimers, shortCircuit: true };
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
