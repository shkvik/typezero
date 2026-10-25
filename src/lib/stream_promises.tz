// node:stream/promises: pipeline() and finished() that give a Promise
// (lib/stream/promises.js of Node 24; docs/streams.md). pipeline() takes a
// list of streams and functions of any types, which the compiler checks and
// gives to the code of node:stream (src/sema/sema_stream.cpp).

import { FinishedOptions, Stream, finishedPromise } from "node:stream"

export function finished(stream: Stream, options?: FinishedOptions): Promise<void> {
  return finishedPromise(stream, options)
}

// The compiler makes each call a call of streamPipelinePromise().
export function pipeline() {}
