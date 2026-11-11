#include "compiler/program/library/crypto/crypto_property_table/crypto_property_table.h"

namespace tz {

namespace {

const CryptoPropertyInfo kCryptoProperties[] = {
    {
        .name = "type",
        .owner = Type::Crypto::KeyObject,
        .property = CryptoProperty::KEY_TYPE,
    },
    {
        .name = "asymmetricKeyType",
        .owner = Type::Crypto::KeyObject,
        .property = CryptoProperty::KEY_ASYMMETRIC_KEY_TYPE,
    },
    {
        .name = "symmetricKeySize",
        .owner = Type::Crypto::KeyObject,
        .property = CryptoProperty::KEY_SYMMETRIC_KEY_SIZE,
    },
    {
        .name = "asymmetricKeyDetails",
        .owner = Type::Crypto::KeyObject,
        .property = CryptoProperty::KEY_ASYMMETRIC_KEY_DETAILS,
    },
    {
        .name = "subject",
        .owner = Type::Crypto::X509,
        .property = CryptoProperty::X509_SUBJECT,
    },
    {
        .name = "issuer",
        .owner = Type::Crypto::X509,
        .property = CryptoProperty::X509_ISSUER,
    },
    {
        .name = "validFrom",
        .owner = Type::Crypto::X509,
        .property = CryptoProperty::X509_VALID_FROM,
    },
    {
        .name = "validTo",
        .owner = Type::Crypto::X509,
        .property = CryptoProperty::X509_VALID_TO,
    },
    {
        .name = "validFromDate",
        .owner = Type::Crypto::X509,
        .property = CryptoProperty::X509_VALID_FROM_DATE,
    },
    {
        .name = "validToDate",
        .owner = Type::Crypto::X509,
        .property = CryptoProperty::X509_VALID_TO_DATE,
    },
    {
        .name = "serialNumber",
        .owner = Type::Crypto::X509,
        .property = CryptoProperty::X509_SERIAL_NUMBER,
    },
    {
        .name = "fingerprint",
        .owner = Type::Crypto::X509,
        .property = CryptoProperty::X509_FINGERPRINT,
    },
    {
        .name = "fingerprint256",
        .owner = Type::Crypto::X509,
        .property = CryptoProperty::X509_FINGERPRINT256,
    },
    {
        .name = "fingerprint512",
        .owner = Type::Crypto::X509,
        .property = CryptoProperty::X509_FINGERPRINT512,
    },
    {
        .name = "subjectAltName",
        .owner = Type::Crypto::X509,
        .property = CryptoProperty::X509_SUBJECT_ALT_NAME,
    },
    {
        .name = "infoAccess",
        .owner = Type::Crypto::X509,
        .property = CryptoProperty::X509_INFO_ACCESS,
    },
    {
        .name = "keyUsage",
        .owner = Type::Crypto::X509,
        .property = CryptoProperty::X509_KEY_USAGE,
    },
    {
        .name = "ca",
        .owner = Type::Crypto::X509,
        .property = CryptoProperty::X509_CA,
    },
    {
        .name = "raw",
        .owner = Type::Crypto::X509,
        .property = CryptoProperty::X509_RAW,
    },
    {
        .name = "publicKey",
        .owner = Type::Crypto::X509,
        .property = CryptoProperty::X509_PUBLIC_KEY,
    },
    {
        .name = "issuerCertificate",
        .owner = Type::Crypto::X509,
        .property = CryptoProperty::X509_ISSUER_CERTIFICATE,
    },
};

}

const CryptoPropertyInfo *CryptoPropertyTable::find(Type::Crypto owner, llvm::StringRef name) {
  for (const CryptoPropertyInfo &info : kCryptoProperties) {
    if (info.owner == owner && info.name == name) {
      return &info;
    }
  }

  return nullptr;
}

}
