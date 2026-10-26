// "node:child_process" for the tests that run under Node too (see
// preload.cjs): execSync, execFileSync and spawnSync give a Result in tz,
// with an ExecError object (docs/process.md, section 1), and here too.
import * as cp from "node:child_process";

export * from "node:child_process";

const fields = ["errno", "syscall", "path", "spawnargs", "status", "signal", "stdout", "stderr"];

function execError(e, ran) {
  const error = { code: typeof e.code === "string" ? e.code : "exit", message: e.message };
  for (const name of fields) {
    const value = e[name] ?? ran?.[name];
    if (value !== undefined && value !== null)
      error[name] = value;
  }
  return error;
}

function resulting(f) {
  return (...args) => {
    try {
      return ok(f(...args));
    } catch (e) {
      return err(execError(e));
    }
  };
}

export const execSync = resulting(cp.execSync);
export const execFileSync = resulting(cp.execFileSync);

export function spawnSync(...args) {
  let ran;
  try {
    ran = cp.spawnSync(...args);
  } catch (e) {
    return err(execError(e));
  }
  if (ran.error)
    return err(execError(ran.error, ran));
  return ok(ran);
}
