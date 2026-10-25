// Runs before every `// same-as-node` test under Node (see check.cmake): it
// gives Node what tz has built in and Node has not (docs/io.md).
//
// - SystemError: Node has no global of that name, only the errors of the
//   system it describes. Here `e instanceof SystemError` holds for an Error
//   with a string `code` and `syscall`, which those errors have.
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

globalThis.SystemError = class SystemError extends Error {
  static [Symbol.hasInstance](value) {
    return value instanceof Error && typeof value.code === "string" &&
           typeof value.syscall === "string";
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
