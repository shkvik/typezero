import { createHash } from 'node:crypto';

// tz: the bytes are a Uint8Array, as the type says: no branches for an array or a string.
function sha1(bytes: Uint8Array): Uint8Array {
  return createHash('sha1').update(bytes).digest();
}

export { sha1 };
