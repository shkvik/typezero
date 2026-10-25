import { md5 } from './md5.js';
import { UUIDTypes } from './types.js';
import { v35 } from './v35.js';

export { DNS, URL } from './v35.js';

function v3<TBuf extends Uint8Array = never>(
  value: string | Uint8Array,
  namespace: UUIDTypes,
  buf?: TBuf,
  offset?: number,
): UUIDTypes<TBuf> {
  return v35(0x30, md5, value, namespace, buf, offset);
}

export { v3 };
