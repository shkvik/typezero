// Names of hashes and ciphers written in the code are checked when compiling (docs/crypto.md,
// section 5): node would throw when the program runs.
import { createHash, createHmac, createCipheriv, hash, pbkdf2Sync, createSign } from "node:crypto"

// expect-error: unknown hash 'sha265'; node has those getHashes() lists, e.g. 'sha256', 'sha512', 'sha3-256', 'md5'
createHash("sha265")
// expect-error: 'md4' is turned off in node 24 (it throws ERR_OSSL_EVP_UNSUPPORTED) and in tz; use 'sha256'
createHash("md4")
// expect-error: 'whirlpool' is turned off in node 24
hash("whirlpool", "data")
// expect-error: 'shake128' gives output of any length, which HMAC and the KDFs cannot use; use a hash such as 'sha256'
createHmac("shake128", "key")
// expect-error: unknown hash 'sha512/224'
pbkdf2Sync("password", "salt", 1000, 32, "sha512/224")
// expect-error: unknown hash 'foo'
createSign("foo")
// expect-error: unknown cipher 'aes-256-gcn'; node has those getCiphers() lists, e.g. 'aes-256-gcm', 'aes-256-cbc', 'chacha20-poly1305'
createCipheriv("aes-256-gcn", Buffer.alloc(32), Buffer.alloc(12))
// expect-error: 'des-cbc' is turned off in node 24 (it throws ERR_OSSL_EVP_UNSUPPORTED) and in tz; use 'aes-256-gcm'
createCipheriv("des-cbc", Buffer.alloc(8), Buffer.alloc(8))
// expect-error: 'bf-cbc' is turned off in node 24
createCipheriv("bf-cbc", Buffer.alloc(16), Buffer.alloc(8))
// expect-error: 'aes-128-ccm' needs the length of its tag: '{ authTagLength: 16 }'
createCipheriv("aes-128-ccm", Buffer.alloc(16), Buffer.alloc(12))
