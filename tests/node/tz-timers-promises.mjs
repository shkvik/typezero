// "node:timers/promises" for the tests that run under Node too (see
// preload.cjs): a wait given a signal gives a Promise of a Result in tz, an
// Err when the signal aborts it (docs/errors.md, section 4), and an
// interval stops at the abort.
import * as timers from "node:timers/promises";

export * from "node:timers/promises";

function settled(promise, signal) {
  if (!signal) return promise;
  return promise.then(
    (value) => ok(value),
    (e) => {
      if (e?.code !== "ABORT_ERR") throw e;
      return err({ code: e.code, message: e.message });
    },
  );
}

export function setTimeout(ms, value, options) {
  return settled(timers.setTimeout(ms, value, options), options?.signal);
}

export function setImmediate(value, options) {
  return settled(timers.setImmediate(value, options), options?.signal);
}

export async function* setInterval(ms, value, options) {
  try {
    for await (const tick of timers.setInterval(ms, value, options)) yield tick;
  } catch (e) {
    if (e?.code !== "ABORT_ERR") throw e;
  }
}

export const scheduler = {
  wait: (ms, options) => settled(timers.scheduler.wait(ms, options), options?.signal),
  yield: () => timers.scheduler.yield(),
};
