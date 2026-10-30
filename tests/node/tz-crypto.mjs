// "node:crypto" for the tests that run under Node too (see preload.cjs): what
// may fail on the data gives a Result of a CryptoError in tz (docs/crypto.md,
// section 5), and here too. Objects that tz gives without a Result when the
// algorithm is a literal (Hash, Hmac, Sign, Verify, ECDH) answer as an Ok
// too, so the same test reads them both ways.
import * as crypto from "node:crypto";

export * from "node:crypto";

const codes = {
  "Unsupported state or unable to authenticate data": "ERR_CRYPTO_AUTH_FAILED",
  "Trying to add data in unsupported state": "ERR_CRYPTO_INVALID_STATE",
};

function cryptoError(e) {
  const code = e.code ?? codes[e.message] ??
    (e.message.startsWith("Digest method") ? "ERR_CRYPTO_INVALID_DIGEST" : "ERR_CRYPTO_OPERATION_FAILED");
  return { code, message: e.message };
}

function resulting(f) {
  return function (...args) {
    try {
      return ok(f.apply(this, args));
    } catch (e) {
      return err(cryptoError(e));
    }
  };
}

function maybe(f) {
  return function (...args) {
    try {
      return f.apply(this, args);
    } catch (e) {
      return err(cryptoError(e));
    }
  };
}

function okToo(proto) {
  Object.defineProperties(proto, {
    isOk: { value() { return true; }, configurable: true },
    isErr: { value() { return false; }, configurable: true },
    value: { get() { return this; }, configurable: true },
    map: { value(f) { return ok(f(this)); }, configurable: true },
    mapErr: { value() { return this; }, configurable: true },
    andThen: { value(f) { return f(this); }, configurable: true },
    match: { value(onOk) { return onOk(this); }, configurable: true },
    unwrapOr: { value() { return this; }, configurable: true },
  });
}

const key = Buffer.alloc(32);
const hashProto = Object.getPrototypeOf(crypto.createHash("sha256"));
const hmacProto = Object.getPrototypeOf(crypto.createHmac("sha256", "k"));
const cipherProto = Object.getPrototypeOf(crypto.createCipheriv("aes-256-gcm", key, Buffer.alloc(12)));
const decipherProto = Object.getPrototypeOf(crypto.createDecipheriv("aes-256-gcm", key, Buffer.alloc(12)));
const signProto = Object.getPrototypeOf(crypto.createSign("sha256"));
const verifyProto = Object.getPrototypeOf(crypto.createVerify("sha256"));
const ecdhProto = Object.getPrototypeOf(crypto.createECDH("prime256v1"));
const pair = crypto.generateKeyPairSync("ed25519");
const keyProtos = new Set();
for (const made of [crypto.createSecretKey(key), pair.publicKey, pair.privateKey]) {
  for (let proto = Object.getPrototypeOf(made); proto; proto = Object.getPrototypeOf(proto)) {
    if (Object.hasOwn(proto, "export"))
      keyProtos.add(proto);
  }
}
const x509Proto = crypto.X509Certificate.prototype;
for (const proto of [hashProto, hmacProto, signProto, verifyProto, ecdhProto])
  okToo(proto);

const digest = hashProto.digest;
const copy = hashProto.copy;
hashProto.digest = function (encoding) {
  const length = this.__length;
  return digest.call(copy.call(this, length === undefined ? undefined : { outputLength: length }), encoding);
};
hashProto.copy = function (options) {
  if (options === undefined)
    return copy.call(this);
  return resulting(copy).call(this, options);
};

for (const proto of [cipherProto, decipherProto]) {
  const update = proto.update, final = proto.final;
  const setAAD = proto.setAAD, setAutoPadding = proto.setAutoPadding;
  const remember = (cipher, e) => { cipher.__problem ??= cryptoError(e); };
  proto.update = function (...args) {
    try {
      return update.apply(this, args);
    } catch (e) {
      remember(this, e);
      return typeof args[2] === "string" ? "" : Buffer.alloc(0);
    }
  };
  proto.final = function (...args) {
    if (this.__problem && !this.__finished) {
      this.__finished = true;
      return err(this.__problem);
    }
    try {
      return ok(final.apply(this, args));
    } catch (e) {
      return err(cryptoError(e));
    } finally {
      this.__finished = true;
    }
  };
  proto.setAAD = function (...args) {
    try {
      setAAD.apply(this, args);
    } catch (e) {
      remember(this, e);
    }
    return this;
  };
  proto.setAutoPadding = function (...args) {
    try {
      setAutoPadding.apply(this, args);
    } catch (e) {
      remember(this, e);
    }
    return this;
  };
}
cipherProto.getAuthTag = resulting(cipherProto.getAuthTag);
const setAuthTag = decipherProto.setAuthTag;
decipherProto.setAuthTag = function (...args) {
  try {
    setAuthTag.apply(this, args);
  } catch (e) {
    this.__problem ??= cryptoError(e);
  }
  return this;
};
signProto.sign = resulting(signProto.sign);
verifyProto.verify = resulting(verifyProto.verify);
const getPublicKey = ecdhProto.getPublicKey;
const generateKeys = ecdhProto.generateKeys;
ecdhProto.generateKeys = function (...args) {
  this.getPublicKey = getPublicKey;
  try {
    return generateKeys.apply(this, args);
  } finally {
    delete this.getPublicKey;
  }
};
ecdhProto.getPublicKey = resulting(getPublicKey);
ecdhProto.getPrivateKey = resulting(ecdhProto.getPrivateKey);
ecdhProto.computeSecret = resulting(ecdhProto.computeSecret);
const setPrivateKey = ecdhProto.setPrivateKey;
ecdhProto.setPrivateKey = function (...args) {
  try {
    setPrivateKey.apply(this, args);
    return ok(undefined);
  } catch (e) {
    return err(cryptoError(e));
  }
};
for (const proto of keyProtos)
  proto.export = resulting(proto.export);
x509Proto.checkPrivateKey = resulting(x509Proto.checkPrivateKey);
x509Proto.verify = resulting(x509Proto.verify);
crypto.X509Certificate.parse = resulting((data) => new crypto.X509Certificate(data));

export function createHash(name, options) {
  try {
    const hash = crypto.createHash(name, options);
    hash.__length = options?.outputLength;
    return hash;
  } catch (e) {
    return err(cryptoError(e));
  }
}
export const createHmac = maybe(crypto.createHmac);
export const createSign = maybe(crypto.createSign);
export const createVerify = maybe(crypto.createVerify);
export const createECDH = maybe(crypto.createECDH);
export const createCipheriv = resulting(crypto.createCipheriv);
export const createDecipheriv = resulting(crypto.createDecipheriv);
export const createPrivateKey = resulting(crypto.createPrivateKey);
export const createPublicKey = resulting(crypto.createPublicKey);
export const generateKeyPairSync = resulting(crypto.generateKeyPairSync);
export const sign = resulting(crypto.sign);
export const verify = resulting(crypto.verify);
export const diffieHellman = resulting(crypto.diffieHellman);
export const pbkdf2Sync = resulting(crypto.pbkdf2Sync);
export const hkdfSync = resulting(crypto.hkdfSync);
export const scryptSync = resulting(crypto.scryptSync);
export const argon2Sync = resulting(crypto.argon2Sync);
export const timingSafeEqual = (a, b) => a.byteLength === b.byteLength && crypto.timingSafeEqual(a, b);
export const hash = resulting(crypto.hash);
