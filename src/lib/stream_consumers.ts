// node:stream/consumers: the whole content of a stream, as text, JSON, a
// Buffer or an ArrayBuffer (lib/stream/consumers.js of Node 24;
// docs/streams.md).

import { Stream, streamChunks } from "node:stream"
import { StringDecoder } from "node:string_decoder"

// node decodes with a TextDecoder; a StringDecoder of UTF-8 gives the same
// text, and replaces bad bytes as Buffer.toString() does.
export async function text(stream: Stream): Promise<string> {
  const dec = new StringDecoder("utf8")
  let str = ""
  for await (const chunk of streamChunks(stream)) {
    if (typeof chunk == "string") str += chunk
    else str += dec.write(__tz_chunk<Buffer>(chunk))
  }
  str += dec.end()
  return str
}

export async function json(stream: Stream): Promise<Json> {
  return JSON.parse(await text(stream))
}

export async function buffer(stream: Stream): Promise<Buffer> {
  const chunks: Buffer[] = []
  for await (const chunk of streamChunks(stream)) {
    if (typeof chunk == "string") chunks.push(Buffer.from(chunk))
    else chunks.push(__tz_chunk<Buffer>(chunk))
  }
  return Buffer.concat(chunks)
}

export async function arrayBuffer(stream: Stream): Promise<ArrayBuffer> {
  const all = await buffer(stream)
  const copy = new Uint8Array(all.length)
  copy.set(all)
  return copy.buffer
}
