// uuid 14.0.3 (https://github.com/uuidjs/uuid, MIT), adapted for tz: 'tz port' made '===' '==',
// 'export default' named exports, 'import type' plain imports; by hand, after the list it
// printed and what 'tz build' reported:
// - v1, v3, v4, v5, v6, v7, v1ToV6 and v6ToV1 have one signature with unions: plain functions
//   have no overloads (docs/overloads.md), so v4() is string | Uint8Array (UUIDTypes), as
//   ms("1h") is number | string; a buffer to write into is a Uint8Array, not a generic TBuf;
// - v3.DNS and v5.DNS (properties of functions, which tz has not) are DNS and URL of this file;
// - NonSharedArrayBuffer is Uint8Array (no ReturnType<typeof Uint8Array.of>);
// - v4 calls crypto.randomUUID() without asking whether it is there;
// - stringToBytes is TextEncoder, as its TODO says (no unescape / encodeURIComponent);
// - md5 and sha1 take a Uint8Array, as their type says;
// - the bytes are written into an array of 16 and then into `buf` with set(), and read with
//   '?? 0' where tz cannot prove the index; '>>>' takes the int of a number ('x | 0').
// node runs it as it is (it strips the types).
export { max as MAX } from './max.js';
export { nil as NIL } from './nil.js';
export { parse as parse } from './parse.js';
export { stringify as stringify } from './stringify.js';
export * from './types.js';
export { v1 as v1 } from './v1.js';
export { v1ToV6 as v1ToV6 } from './v1ToV6.js';
export { v3 as v3 } from './v3.js';
export { v4 as v4 } from './v4.js';
export { v5 as v5 } from './v5.js';
export { v6 as v6 } from './v6.js';
export { v6ToV1 as v6ToV1 } from './v6ToV1.js';
export { v7 as v7 } from './v7.js';
export { validate as validate } from './validate.js';
export { version as version } from './version.js';
export { DNS, URL } from './v35.js';
