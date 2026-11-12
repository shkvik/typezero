#include "compiler/program/library/crypto/curve_name_table/curve_name_table.h"

namespace tz {

namespace {

const CurveNameInfo kCurves[] = {
    {.name = "Oakley-EC2N-3", .isWebCrypto = false},
    {.name = "Oakley-EC2N-4", .isWebCrypto = false},
    {.name = "SM2", .isWebCrypto = false},
    {.name = "brainpoolP160r1", .isWebCrypto = false},
    {.name = "brainpoolP160t1", .isWebCrypto = false},
    {.name = "brainpoolP192r1", .isWebCrypto = false},
    {.name = "brainpoolP192t1", .isWebCrypto = false},
    {.name = "brainpoolP224r1", .isWebCrypto = false},
    {.name = "brainpoolP224t1", .isWebCrypto = false},
    {.name = "brainpoolP256r1", .isWebCrypto = false},
    {.name = "brainpoolP256t1", .isWebCrypto = false},
    {.name = "brainpoolP320r1", .isWebCrypto = false},
    {.name = "brainpoolP320t1", .isWebCrypto = false},
    {.name = "brainpoolP384r1", .isWebCrypto = false},
    {.name = "brainpoolP384t1", .isWebCrypto = false},
    {.name = "brainpoolP512r1", .isWebCrypto = false},
    {.name = "brainpoolP512t1", .isWebCrypto = false},
    {.name = "c2pnb163v1", .isWebCrypto = false},
    {.name = "c2pnb163v2", .isWebCrypto = false},
    {.name = "c2pnb163v3", .isWebCrypto = false},
    {.name = "c2pnb176v1", .isWebCrypto = false},
    {.name = "c2pnb208w1", .isWebCrypto = false},
    {.name = "c2pnb272w1", .isWebCrypto = false},
    {.name = "c2pnb304w1", .isWebCrypto = false},
    {.name = "c2pnb368w1", .isWebCrypto = false},
    {.name = "c2tnb191v1", .isWebCrypto = false},
    {.name = "c2tnb191v2", .isWebCrypto = false},
    {.name = "c2tnb191v3", .isWebCrypto = false},
    {.name = "c2tnb239v1", .isWebCrypto = false},
    {.name = "c2tnb239v2", .isWebCrypto = false},
    {.name = "c2tnb239v3", .isWebCrypto = false},
    {.name = "c2tnb359v1", .isWebCrypto = false},
    {.name = "c2tnb431r1", .isWebCrypto = false},
    {.name = "prime192v1", .isWebCrypto = false},
    {.name = "prime192v2", .isWebCrypto = false},
    {.name = "prime192v3", .isWebCrypto = false},
    {.name = "prime239v1", .isWebCrypto = false},
    {.name = "prime239v2", .isWebCrypto = false},
    {.name = "prime239v3", .isWebCrypto = false},
    {.name = "prime256v1", .isWebCrypto = false},
    {.name = "secp112r1", .isWebCrypto = false},
    {.name = "secp112r2", .isWebCrypto = false},
    {.name = "secp128r1", .isWebCrypto = false},
    {.name = "secp128r2", .isWebCrypto = false},
    {.name = "secp160k1", .isWebCrypto = false},
    {.name = "secp160r1", .isWebCrypto = false},
    {.name = "secp160r2", .isWebCrypto = false},
    {.name = "secp192k1", .isWebCrypto = false},
    {.name = "secp224k1", .isWebCrypto = false},
    {.name = "secp224r1", .isWebCrypto = false},
    {.name = "secp256k1", .isWebCrypto = false},
    {.name = "secp384r1", .isWebCrypto = false},
    {.name = "secp521r1", .isWebCrypto = false},
    {.name = "sect113r1", .isWebCrypto = false},
    {.name = "sect113r2", .isWebCrypto = false},
    {.name = "sect131r1", .isWebCrypto = false},
    {.name = "sect131r2", .isWebCrypto = false},
    {.name = "sect163k1", .isWebCrypto = false},
    {.name = "sect163r1", .isWebCrypto = false},
    {.name = "sect163r2", .isWebCrypto = false},
    {.name = "sect193r1", .isWebCrypto = false},
    {.name = "sect193r2", .isWebCrypto = false},
    {.name = "sect233k1", .isWebCrypto = false},
    {.name = "sect233r1", .isWebCrypto = false},
    {.name = "sect239k1", .isWebCrypto = false},
    {.name = "sect283k1", .isWebCrypto = false},
    {.name = "sect283r1", .isWebCrypto = false},
    {.name = "sect409k1", .isWebCrypto = false},
    {.name = "sect409r1", .isWebCrypto = false},
    {.name = "sect571k1", .isWebCrypto = false},
    {.name = "sect571r1", .isWebCrypto = false},
    {.name = "wap-wsg-idm-ecid-wtls1", .isWebCrypto = false},
    {.name = "wap-wsg-idm-ecid-wtls10", .isWebCrypto = false},
    {.name = "wap-wsg-idm-ecid-wtls11", .isWebCrypto = false},
    {.name = "wap-wsg-idm-ecid-wtls12", .isWebCrypto = false},
    {.name = "wap-wsg-idm-ecid-wtls3", .isWebCrypto = false},
    {.name = "wap-wsg-idm-ecid-wtls4", .isWebCrypto = false},
    {.name = "wap-wsg-idm-ecid-wtls5", .isWebCrypto = false},
    {.name = "wap-wsg-idm-ecid-wtls6", .isWebCrypto = false},
    {.name = "wap-wsg-idm-ecid-wtls7", .isWebCrypto = false},
    {.name = "wap-wsg-idm-ecid-wtls8", .isWebCrypto = false},
    {.name = "wap-wsg-idm-ecid-wtls9", .isWebCrypto = false},
    {.name = "P-192", .isWebCrypto = false},
    {.name = "P-224", .isWebCrypto = false},
    {.name = "P-256", .isWebCrypto = true},
    {.name = "P-384", .isWebCrypto = true},
    {.name = "P-521", .isWebCrypto = true},
};

}

bool CurveNameTable::isKnown(llvm::StringRef name) {
  for (const CurveNameInfo &info : kCurves) {
    if (info.name == name) {
      return true;
    }
  }

  return false;
}

bool CurveNameTable::isWebCryptoCurve(llvm::StringRef name) {
  for (const CurveNameInfo &info : kCurves) {
    if (info.name == name) {
      return info.isWebCrypto;
    }
  }

  return false;
}

}
