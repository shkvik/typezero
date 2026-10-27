// "node:stream/promises" for the tests that run under Node too (see
// preload.cjs): pipeline() and finished() give a Promise of a Result in tz,
// an Err with the first error of the streams (docs/streams.md, section 3).
import * as promises from "node:stream/promises";

export * from "node:stream/promises";

export async function pipeline(...stages) {
  try {
    return ok(await promises.pipeline(...stages));
  } catch (e) {
    return err(e);
  }
}

export async function finished(stream, options) {
  try {
    return ok(await promises.finished(stream, options));
  } catch (e) {
    return err(e);
  }
}
