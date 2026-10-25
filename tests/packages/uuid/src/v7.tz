import { rng } from './rng.js';
import { unsafeStringify } from './stringify.js';
import { UUIDTypes, Version7Options } from './types.js';

type V7State = {
  msecs?: number; // time, milliseconds
  seq?: number; // sequence number (32-bits)
};

const _state: V7State = {};

function v7<TBuf extends Uint8Array = never>(
  options?: Version7Options,
  buf?: TBuf,
  offset?: number,
): UUIDTypes<TBuf> {
  let bytes: Uint8Array;

  if (options) {
    // With options: Make UUID independent of internal state
    bytes = v7Bytes(
      options.random ?? options.rng?.() ?? rng(),
      options.msecs,
      options.seq,
      buf,
      offset,
    );
  } else {
    // No options: Use internal state
    const now = Date.now();
    const rnds = rng();

    updateV7State(_state, now, rnds);

    bytes = v7Bytes(rnds, _state.msecs, _state.seq, buf, offset);
  }

  return buf ?? unsafeStringify(bytes);
}

// (Private!)  Do not use.  This method is only exported for testing purposes
// and may change without notice.
export function updateV7State(state: V7State, now: number, rnds: Uint8Array) {
  let msecs = state.msecs ?? -Infinity;
  let seq = state.seq ?? 0;

  if (now > msecs) {
    // Time has moved on! Pick a new random sequence number
    seq = v7Sequence(rnds);
    msecs = now;
  } else {
    // Bump sequence counter w/ 32-bit rollover
    seq = (seq + 1) | 0;

    // In case of rollover, bump timestamp to preserve monotonicity. This is
    // allowed by the RFC and should self-correct as the system clock catches
    // up. See https://www.rfc-editor.org/rfc/rfc9562.html#section-6.2-9.4
    if (seq == 0) {
      msecs++;
    }
  }
  state.msecs = msecs;
  state.seq = seq;

  return state;
}

function v7Bytes(
  rnds: Uint8Array,
  msecs?: number,
  seq?: number,
  buf?: Uint8Array,
  offset: number = 0,
) {
  if (rnds.length < 16) {
    throw new Error('Random bytes length must be >= 16');
  }

  if (buf) {
    if (offset < 0 || offset + 16 > buf.length) {
      throw new RangeError(
        `UUID byte range ${offset}:${offset + 15} is out of buffer bounds`,
      );
    }
  }

  // Defaults
  msecs ??= Date.now();
  seq ??= v7Sequence(rnds);

  // tz: the 16 bytes go into an array of their own, whose indices need no checks, and then
  // into `buf` at `offset`, which was checked above; `>>>` takes the int of `seq`.
  const b = new Uint8Array(16);
  const s = seq | 0;

  // byte 0-5: timestamp (48 bits)
  b[0] = (msecs / 0x10000000000) & 0xff;
  b[1] = (msecs / 0x100000000) & 0xff;
  b[2] = (msecs / 0x1000000) & 0xff;
  b[3] = (msecs / 0x10000) & 0xff;
  b[4] = (msecs / 0x100) & 0xff;
  b[5] = msecs & 0xff;

  // byte 6: `version` (4 bits) | sequence bits 28-31 (4 bits)
  b[6] = 0x70 | ((s >>> 28) & 0x0f);

  // byte 7: sequence bits 20-27 (8 bits)
  b[7] = (s >>> 20) & 0xff;

  // byte 8: `variant` (2 bits) | sequence bits 14-19 (6 bits)
  b[8] = 0x80 | ((s >>> 14) & 0x3f);

  // byte 9: sequence bits 6-13 (8 bits)
  b[9] = (s >>> 6) & 0xff;

  // byte 10: sequence bits 0-5 (6 bits) | random (2 bits)
  b[10] = ((s << 2) & 0xff) | (rnds[10] & 0x03);

  // bytes 11-15: random (40 bits)
  b[11] = rnds[11];
  b[12] = rnds[12];
  b[13] = rnds[13];
  b[14] = rnds[14];
  b[15] = rnds[15];

  if (!buf) return b;
  buf.set(b, offset | 0);
  return buf;
}

function v7Sequence(rnds: Uint8Array) {
  return (
    (((rnds[6] ?? 0) & 0x7f) << 24) |
    ((rnds[7] ?? 0) << 16) |
    ((rnds[8] ?? 0) << 8) |
    (rnds[9] ?? 0)
  );
}

export { v7 };
