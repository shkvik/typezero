import { parse } from './parse.js';
import { unsafeStringify } from './stringify.js';
import { UUIDTypes, Version6Options } from './types.js';
import { v1 } from './v1.js';
import { v1ToV6 } from './v1ToV6.js';

// tz: v1() and v1ToV6() give a string or bytes (UUIDTypes), as plain functions have one
// signature; bytes in, bytes out, which a string would only be after parse().
function bytesOf(uuid: UUIDTypes): Uint8Array {
  return typeof uuid == 'string' ? parse(uuid) : uuid;
}

function v6<TBuf extends Uint8Array = never>(
  options?: Version6Options,
  buf?: TBuf,
  offset?: number,
): UUIDTypes<TBuf> {
  options ??= {};
  offset ??= 0;

  // v6 is v1 with different field layout, so we start with a v1 UUID, albeit
  // with slightly different behavior around how the clock_seq and node fields
  // are randomized, which is why we call v1 with _v6: true.
  let bytes = bytesOf(v1({ ...options, _v6: true }, new Uint8Array(16)));

  // Reorder the fields to v6 layout.
  bytes = bytesOf(v1ToV6(bytes));

  // Return as a byte array if requested
  if (buf) {
    if (offset < 0 || offset + 16 > buf.length) {
      throw new RangeError(
        `UUID byte range ${offset}:${offset + 15} is out of buffer bounds`,
      );
    }

    buf.set(bytes.subarray(0, 16), offset | 0);
    return buf;
  }

  return unsafeStringify(bytes);
}

export { v6 };
