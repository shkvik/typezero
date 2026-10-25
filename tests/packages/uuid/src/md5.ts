import { createHash } from 'node:crypto';

// tz: the bytes are a Uint8Array, as the type says: no branches for an array or a string.
function md5(bytes: Uint8Array): Uint8Array {
  return createHash('md5').update(bytes).digest();
}

export { md5 };
