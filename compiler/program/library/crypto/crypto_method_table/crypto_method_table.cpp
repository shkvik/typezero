#include "compiler/program/library/crypto/crypto_method_table/crypto_method_table.h"

namespace tz {

namespace {

const CryptoMethodInfo kCryptoMethods[] = {
    {
        .name = "update",
        .owner = Type::Crypto::Hash,
        .method = CryptoMethod::HASH_UPDATE,
    },
    {
        .name = "update",
        .owner = Type::Crypto::Hmac,
        .method = CryptoMethod::HASH_UPDATE,
    },
    {
        .name = "update",
        .owner = Type::Crypto::Sign,
        .method = CryptoMethod::HASH_UPDATE,
    },
    {
        .name = "update",
        .owner = Type::Crypto::Verify,
        .method = CryptoMethod::HASH_UPDATE,
    },
    {
        .name = "digest",
        .owner = Type::Crypto::Hash,
        .method = CryptoMethod::DIGEST,
    },
    {
        .name = "digest",
        .owner = Type::Crypto::Hmac,
        .method = CryptoMethod::DIGEST,
    },
    {
        .name = "copy",
        .owner = Type::Crypto::Hash,
        .method = CryptoMethod::HASH_COPY,
    },
    {
        .name = "sign",
        .owner = Type::Crypto::Sign,
        .method = CryptoMethod::SIGN,
    },
    {
        .name = "verify",
        .owner = Type::Crypto::Verify,
        .method = CryptoMethod::VERIFY,
    },
    {
        .name = "update",
        .owner = Type::Crypto::Cipheriv,
        .method = CryptoMethod::CIPHER_UPDATE,
    },
    {
        .name = "update",
        .owner = Type::Crypto::Decipheriv,
        .method = CryptoMethod::CIPHER_UPDATE,
    },
    {
        .name = "final",
        .owner = Type::Crypto::Cipheriv,
        .method = CryptoMethod::CIPHER_FINAL,
    },
    {
        .name = "final",
        .owner = Type::Crypto::Decipheriv,
        .method = CryptoMethod::CIPHER_FINAL,
    },
    {
        .name = "setAAD",
        .owner = Type::Crypto::Cipheriv,
        .method = CryptoMethod::SET_AAD,
    },
    {
        .name = "setAAD",
        .owner = Type::Crypto::Decipheriv,
        .method = CryptoMethod::SET_AAD,
    },
    {
        .name = "getAuthTag",
        .owner = Type::Crypto::Cipheriv,
        .method = CryptoMethod::GET_AUTH_TAG,
    },
    {
        .name = "setAuthTag",
        .owner = Type::Crypto::Decipheriv,
        .method = CryptoMethod::SET_AUTH_TAG,
    },
    {
        .name = "setAutoPadding",
        .owner = Type::Crypto::Cipheriv,
        .method = CryptoMethod::SET_AUTO_PADDING,
    },
    {
        .name = "setAutoPadding",
        .owner = Type::Crypto::Decipheriv,
        .method = CryptoMethod::SET_AUTO_PADDING,
    },
    {
        .name = "export",
        .owner = Type::Crypto::KeyObject,
        .method = CryptoMethod::KEY_EXPORT,
    },
    {
        .name = "equals",
        .owner = Type::Crypto::KeyObject,
        .method = CryptoMethod::KEY_EQUALS,
    },
    {
        .name = "generateKeys",
        .owner = Type::Crypto::Ecdh,
        .method = CryptoMethod::ECDH_GENERATE_KEYS,
    },
    {
        .name = "getPublicKey",
        .owner = Type::Crypto::Ecdh,
        .method = CryptoMethod::ECDH_GET_PUBLIC_KEY,
    },
    {
        .name = "getPrivateKey",
        .owner = Type::Crypto::Ecdh,
        .method = CryptoMethod::ECDH_GET_PRIVATE_KEY,
    },
    {
        .name = "setPrivateKey",
        .owner = Type::Crypto::Ecdh,
        .method = CryptoMethod::ECDH_SET_PRIVATE_KEY,
    },
    {
        .name = "computeSecret",
        .owner = Type::Crypto::Ecdh,
        .method = CryptoMethod::ECDH_COMPUTE_SECRET,
    },
    {
        .name = "toString",
        .owner = Type::Crypto::X509,
        .method = CryptoMethod::X509_PEM,
    },
    {
        .name = "toJSON",
        .owner = Type::Crypto::X509,
        .method = CryptoMethod::X509_PEM,
    },
    {
        .name = "checkHost",
        .owner = Type::Crypto::X509,
        .method = CryptoMethod::X509_CHECK_HOST,
    },
    {
        .name = "checkEmail",
        .owner = Type::Crypto::X509,
        .method = CryptoMethod::X509_CHECK_EMAIL,
    },
    {
        .name = "checkIP",
        .owner = Type::Crypto::X509,
        .method = CryptoMethod::X509_CHECK_IP,
    },
    {
        .name = "checkIssued",
        .owner = Type::Crypto::X509,
        .method = CryptoMethod::X509_CHECK_ISSUED,
    },
    {
        .name = "checkPrivateKey",
        .owner = Type::Crypto::X509,
        .method = CryptoMethod::X509_CHECK_PRIVATE_KEY,
    },
    {
        .name = "verify",
        .owner = Type::Crypto::X509,
        .method = CryptoMethod::X509_VERIFY,
    },
};

}

const CryptoMethodInfo *CryptoMethodTable::find(Type::Crypto owner, llvm::StringRef name) {
  for (const CryptoMethodInfo &info : kCryptoMethods) {
    if (info.owner == owner && info.name == name) {
      return &info;
    }
  }

  return nullptr;
}

}
