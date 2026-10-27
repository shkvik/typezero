// "node:stream/consumers" for the tests that run under Node too (see
// preload.cjs): text(), json(), buffer() and arrayBuffer() give a Promise of
// a Result in tz, an Err when the stream fails (docs/streams.md, section 3).
import * as consumers from "node:stream/consumers";

export * from "node:stream/consumers";

function resulting(f) {
  return async (stream) => {
    try {
      return ok(await f(stream));
    } catch (e) {
      return err(e);
    }
  };
}

export const text = resulting(consumers.text);
export const buffer = resulting(consumers.buffer);
export const arrayBuffer = resulting(consumers.arrayBuffer);

export async function json(stream) {
  const content = await text(stream);
  return content.isErr() ? content : JSON.parse(content.value);
}
