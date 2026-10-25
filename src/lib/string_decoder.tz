// node:string_decoder: StringDecoder, which turns a series of Buffers into
// strings without cutting a character in two (docs/streams.md, section 2).
// Node 24 decodes in C++ (src/string_decoder.cc); this is a port of that
// code to tz: the bytes of a character that a chunk cuts off wait in
// `lastChar` for the next one, and what is complete is decoded as
// Buffer.toString() decodes it, with the same replacement of bad bytes.

// node's ERR_UNKNOWN_ENCODING.
class UnknownEncodingError extends TypeError {
  code = "ERR_UNKNOWN_ENCODING"
  constructor(encoding: string) {
    super(`Unknown encoding: ${encoding}`)
  }
}

// node's normalizeEncoding (lib/internal/util.js): the name Buffer knows an
// encoding by, or "" for none.
export function normalizeEncoding(encoding: string | undefined): string {
  if (encoding == undefined || encoding == "") return "utf8"
  const lower = encoding.toLowerCase()
  switch (lower) {
    case "utf8":
    case "utf-8":
      return "utf8"
    case "ucs2":
    case "ucs-2":
    case "utf16le":
    case "utf-16le":
      return "utf16le"
    case "latin1":
    case "binary":
      return "latin1"
    case "base64":
    case "base64url":
    case "hex":
    case "ascii":
      return lower
    default:
      return ""
  }
}

export class StringDecoder {
  readonly encoding: string
  // The bytes of the character that the last chunk cut off, as node's
  // lastChar; how many it still needs (lastNeed) and how many it has.
  readonly lastChar = Buffer.alloc(4)
  lastNeed: int = 0
  lastTotal: int = 0
  @__tz_hidden buffered: int = 0

  constructor(encoding?: string) {
    const normalized = normalizeEncoding(encoding)
    if (normalized == "") throw new UnknownEncodingError(encoding ?? "")
    this.encoding = normalized
  }

  // The text of the bytes of `buf`, without the end of a character that
  // the next chunk finishes.
  write(buf: Buffer | Uint8Array | string): string {
    if (typeof buf == "string") return buf
    const data = buf instanceof Buffer ? buf : Buffer.from(buf.buffer, buf.byteOffset, buf.length)
    return this.#decode(data)
  }

  // The rest: what write() gives of `buf`, then the bytes still waiting,
  // decoded as they are (an unfinished character of UTF-8 becomes U+FFFD).
  end(buf?: Buffer | Uint8Array | string): string {
    const ret = buf == undefined ? "" : this.write(buf)
    if (this.buffered > 0) return ret + this.#flush()
    return ret
  }

  // Legacy: decodes `buf` from `offset` with nothing waiting.
  text(buf: Buffer, offset: int): string {
    this.#setState(0, 0)
    return this.write(buf.subarray(offset))
  }

  #setState(buffered: int, missing: int) {
    this.buffered = buffered
    this.lastNeed = missing
    this.lastTotal = buffered + missing
  }

  #make(data: Buffer, start: int, end: int): string {
    return data.toString(this.encoding, start, end)
  }

  // StringDecoder::DecodeData of node.
  #decode(data: Buffer): string {
    const encoding = this.encoding
    if (encoding != "utf8" && encoding != "utf16le" && encoding != "base64" &&
        encoding != "base64url")
      return this.#make(data, 0, data.length)
    let start = 0
    let nread = data.length
    let prepend: string | undefined = undefined
    // Bytes that finish a character of the last chunk.
    if (this.lastNeed > 0) {
      if (encoding == "utf8") {
        // A byte that is not a continuation byte, though it should be,
        // starts a new character: the unfinished one takes the bytes
        // before it.
        for (let i = 0; i < nread && i < this.lastNeed; i++) {
          if (((data[start + i] ?? 0) & 0xc0) != 0x80) {
            data.copy(this.lastChar, this.buffered, start, start + i)
            this.#setState(this.buffered + i, 0)
            start += i
            nread -= i
            break
          }
        }
      }
      const found = Math.min(nread, this.lastNeed)
      data.copy(this.lastChar, this.buffered, start, start + found)
      start += found
      nread -= found
      this.#setState(this.buffered + found, this.lastNeed - found)
      if (this.lastNeed == 0) {
        prepend = this.lastChar.toString(encoding, 0, this.buffered)
        this.#setState(0, 0)
      }
    }
    let body: string
    if (nread == 0) {
      body = prepend ?? ""
      prepend = undefined
    } else {
      // A character the chunk may cut off, which the next one finishes.
      let buffered = 0
      let missing = 0
      const last = data[start + nread - 1] ?? 0
      if (encoding == "utf8" && (last & 0x80) != 0) {
        for (let i = nread - 1; ; i--) {
          buffered++
          const byte = data[start + i] ?? 0
          if ((byte & 0xc0) == 0x80) {
            // A trailing byte: four of them, or one at the start of the
            // chunk, belong to a character that is not cut off here.
            if (buffered >= 4 || i == 0) {
              buffered = 0
              break
            }
          } else {
            // The lead byte of a character tells how long it should be.
            if ((byte & 0xe0) == 0xc0) {
              missing = 2
            } else if ((byte & 0xf0) == 0xe0) {
              missing = 3
            } else if ((byte & 0xf8) == 0xf0) {
              missing = 4
            } else {
              buffered = 0
              break
            }
            if (buffered >= missing) {
              missing = 0
              buffered = 0
            }
            missing -= buffered
            break
          }
        }
      } else if (encoding == "utf16le") {
        if (nread % 2 == 1) {
          buffered = 1
          missing = 1
        } else if ((last & 0xfc) == 0xd8) {
          // The first half of a surrogate pair.
          buffered = 2
          missing = 2
        }
      } else if (encoding == "base64" || encoding == "base64url") {
        buffered = nread % 3
        if (buffered > 0) missing = 3 - buffered
      }
      if (buffered > 0) {
        nread -= buffered
        data.copy(this.lastChar, 0, start + nread, start + nread + buffered)
      }
      this.#setState(buffered, missing)
      body = nread > 0 ? this.#make(data, start, start + nread) : ""
    }
    return prepend == undefined ? body : prepend + body
  }

  // StringDecoder::FlushData of node.
  #flush(): string {
    if (this.encoding == "utf16le" && this.buffered % 2 == 1)
      // A single trailing byte is dropped, as the JavaScript decoder does.
      this.#setState(this.buffered - 1, this.lastNeed - 1)
    if (this.buffered == 0) return ""
    const ret = this.lastChar.toString(this.encoding, 0, this.buffered)
    this.#setState(0, 0)
    return ret
  }
}
