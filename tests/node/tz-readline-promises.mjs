// "node:readline/promises" for the tests that run under Node too (see
// preload.cjs): question() gives a Promise of a Result in tz, an Err when its
// signal aborts it or the Interface is closed (docs/io.md, section 5).
import * as readline from "node:readline/promises";

export * from "node:readline/promises";

export function createInterface(...args) {
  const rl = readline.createInterface(...args);
  const question = rl.question.bind(rl);
  rl.question = async (query, options) => {
    try {
      return ok(await question(query, options));
    } catch (e) {
      if (e?.code !== "ABORT_ERR" && e?.code !== "ERR_USE_AFTER_CLOSE") throw e;
      return err(e);
    }
  };
  return rl;
}
