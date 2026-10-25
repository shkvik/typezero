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
// - ExecSyncError (docs/process.md): what execSync and execFileSync throw
//   when the program they ran failed is an Error with the result of
//   spawnSync in it, so `e instanceof ExecSyncError` holds for an Error with
//   an `output` array. One for a program that could not start has no output
//   (null): that one is a SystemError.
// - The module "tz:io", with readLine (tz-io.mjs next to this file).
// - The module "tz:http", with serve over node:http (tz-http.mjs).
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

globalThis.ExecSyncError = class ExecSyncError extends Error {
  static [Symbol.hasInstance](value) {
    return value instanceof Error && Array.isArray(value.output);
  }
};

const tzIo = pathToFileURL(join(__dirname, "tz-io.mjs")).href;
const tzHttp = pathToFileURL(join(__dirname, "tz-http.mjs")).href;
registerHooks({
  resolve(specifier, context, nextResolve) {
    if (specifier === "tz:io")
      return { url: tzIo, shortCircuit: true };
    if (specifier === "tz:http")
      return { url: tzHttp, shortCircuit: true };
    return nextResolve(specifier, context);
  },
});
