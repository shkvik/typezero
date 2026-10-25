import { rng } from './rng.js';
import { unsafeStringify } from './stringify.js';
import { UUIDTypes, Version4Options } from './types.js';

function v4<TBuf extends Uint8Array = never>(
  options?: Version4Options,
  buf?: TBuf,
  offset?: number,
): UUIDTypes<TBuf> {
  if (!buf && !options) {
    return crypto.randomUUID();
  }

  // Putting tail-code that could just go inline here in a separate function to
  // enable compiler optimizations that dramatically improve performance.
  //
  // REF: https://github.com/uuidjs/uuid/issues/892
  return _v4(options, buf, offset);
}

function _v4<TBuf extends Uint8Array = never>(
  options?: Version4Options,
  buf?: TBuf,
  offset?: number,
): UUIDTypes<TBuf> {
  options = options || {};

  const rnds = options.random ?? options.rng?.() ?? rng();
  if (rnds.length < 16) {
    throw new Error('Random bytes length must be >= 16');
  }

  // Per 4.4, set bits for version and `clock_seq_hi_and_reserved`
  rnds[6] = (rnds[6] & 0x0f) | 0x40;
  rnds[8] = (rnds[8] & 0x3f) | 0x80;

  // Copy bytes to buffer, if provided
  if (buf) {
    const at = offset ?? 0;
    if (at < 0 || at + 16 > buf.length) {
      throw new RangeError(
        `UUID byte range ${at}:${at + 15} is out of buffer bounds`,
      );
    }

    buf.set(rnds.subarray(0, 16), at | 0);

    return buf;
  }

  return unsafeStringify(rnds);
}
export { v4 };
