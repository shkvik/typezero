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
//   which give a Result in tz, and RegExp.parse, which only tz has.
// - "node:fs" and "node:fs/promises" whose functions give a Result
//   (tz-fs.mjs, tz-fs-promises.mjs), and "node:child_process" whose
//   execSync, execFileSync and spawnSync give one (tz-child-process.mjs), and
//   "node:crypto" with a Result where the data may be wrong (tz-crypto.mjs).
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
