// "node:fs" for the tests that run under Node too (see preload.cjs): the
// functions that fail with an error of the system give a Result in tz
// (docs/io.md, section 5), and here too.
import * as fs from "node:fs";

export * from "node:fs";

function resulting(f) {
  return (...args) => {
    try {
      return ok(f(...args));
    } catch (e) {
      if (typeof e?.code === "string" && typeof e?.syscall === "string")
        return err(e);
      throw e;
    }
  };
}

export const readFileSync = resulting(fs.readFileSync);
export const writeFileSync = resulting(fs.writeFileSync);
export const appendFileSync = resulting(fs.appendFileSync);
export const accessSync = resulting(fs.accessSync);
export const chmodSync = resulting(fs.chmodSync);
export const truncateSync = resulting(fs.truncateSync);
export const statSync = resulting(fs.statSync);
export const lstatSync = resulting(fs.lstatSync);
export const readdirSync = resulting(fs.readdirSync);
export const mkdirSync = resulting(fs.mkdirSync);
export const mkdtempSync = resulting(fs.mkdtempSync);
export const rmdirSync = resulting(fs.rmdirSync);
export const unlinkSync = resulting(fs.unlinkSync);
export const rmSync = resulting(fs.rmSync);
export const renameSync = resulting(fs.renameSync);
export const copyFileSync = resulting(fs.copyFileSync);
export const realpathSync = resulting(fs.realpathSync);
