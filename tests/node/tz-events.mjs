// "node:events" for the tests that run under Node too (see preload.cjs):
// once() and on() of an emitter give a Result in tz, an Err when an 'error'
// event or the signal comes first (docs/events.md, section 4). The tests
// compared with Node use them only on emitters with an 'error' event or
// with a signal, where tz gives a Result too.
import * as events from "node:events";

export * from "node:events";

export function once(emitter, name, options) {
  const promise = events.once(emitter, name, options);
  if (!(emitter instanceof events.EventEmitter) && !options?.signal) return promise;
  return promise.then((args) => ok(args), (e) => err(e));
}

export function on(emitter, name, options) {
  const iterator = events.on(emitter, name, options);
  return (async function* () {
    try {
      for await (const args of iterator) yield ok(args);
    } catch (e) {
      yield err(e);
    }
  })();
}
