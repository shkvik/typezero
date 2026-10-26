// "node:fs/promises" for the tests that run under Node too (see
// preload.cjs): what fails with an error of the system gives a Promise of a
// Result in tz (docs/io.md, section 5), and here too.
import * as fsp from "node:fs/promises";

export * from "node:fs/promises";

function resulting(f) {
  return async (...args) => {
    try {
      return ok(await f(...args));
    } catch (e) {
      if (typeof e?.code === "string" && typeof e?.syscall === "string")
        return err(e);
      throw e;
    }
  };
}

export const readFile = resulting(fsp.readFile);
export const writeFile = resulting(fsp.writeFile);
export const appendFile = resulting(fsp.appendFile);
export const access = resulting(fsp.access);
export const chmod = resulting(fsp.chmod);
export const truncate = resulting(fsp.truncate);
export const stat = resulting(fsp.stat);
export const lstat = resulting(fsp.lstat);
export const readdir = resulting(fsp.readdir);
export const mkdir = resulting(fsp.mkdir);
export const mkdtemp = resulting(fsp.mkdtemp);
export const rmdir = resulting(fsp.rmdir);
export const unlink = resulting(fsp.unlink);
export const rm = resulting(fsp.rm);
export const rename = resulting(fsp.rename);
export const copyFile = resulting(fsp.copyFile);
export const realpath = resulting(fsp.realpath);
