#include "compiler/program/library/crypto/subtle_family_table/subtle_family_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const std::vector<SubtleKeyFormat> kAsymmetricFormats = {
    SubtleKeyFormat::SPKI, SubtleKeyFormat::PKCS8, SubtleKeyFormat::JWK, SubtleKeyFormat::RAW};

const SubtleFamilyInfo kFamilies[] = {
    {
        .family = SubtleFamily::DIGEST,
        .isSecret = false,
        .formats = kAsymmetricFormats,
        .generate = std::nullopt,
        .import = std::nullopt,
        .unwrap = std::nullopt,
    },
    {
        .family = SubtleFamily::HMAC,
        .isSecret = true,
        .formats = {SubtleKeyFormat::RAW, SubtleKeyFormat::JWK},
        .generate = LibraryFunction::SUBTLE_GENERATE_HMAC,
        .import = LibraryFunction::SUBTLE_IMPORT_HMAC,
        .unwrap = LibraryFunction::SUBTLE_UNWRAP_HMAC,
    },
    {
        .family = SubtleFamily::AES,
        .isSecret = true,
        .formats = {SubtleKeyFormat::RAW, SubtleKeyFormat::JWK},
        .generate = LibraryFunction::SUBTLE_GENERATE_AES,
        .import = LibraryFunction::SUBTLE_IMPORT_AES,
        .unwrap = LibraryFunction::SUBTLE_UNWRAP_AES,
    },
    {
        .family = SubtleFamily::KDF,
        .isSecret = true,
        .formats = {SubtleKeyFormat::RAW},
        .generate = std::nullopt,
        .import = LibraryFunction::SUBTLE_IMPORT_KDF,
        .unwrap = LibraryFunction::SUBTLE_UNWRAP_KDF,
    },
    {
        .family = SubtleFamily::EC,
        .isSecret = false,
        .formats = kAsymmetricFormats,
        .generate = LibraryFunction::SUBTLE_GENERATE_EC,
        .import = LibraryFunction::SUBTLE_IMPORT_EC,
        .unwrap = LibraryFunction::SUBTLE_UNWRAP_EC,
    },
    {
        .family = SubtleFamily::RSA,
        .isSecret = false,
        .formats = {SubtleKeyFormat::SPKI, SubtleKeyFormat::PKCS8, SubtleKeyFormat::JWK},
        .generate = LibraryFunction::SUBTLE_GENERATE_RSA,
        .import = LibraryFunction::SUBTLE_IMPORT_RSA,
        .unwrap = LibraryFunction::SUBTLE_UNWRAP_RSA,
    },
    {
        .family = SubtleFamily::OKP,
        .isSecret = false,
        .formats = kAsymmetricFormats,
        .generate = LibraryFunction::SUBTLE_GENERATE_OKP,
        .import = LibraryFunction::SUBTLE_IMPORT_OKP,
        .unwrap = LibraryFunction::SUBTLE_UNWRAP_OKP,
    },
};

}

const SubtleFamilyInfo &SubtleFamilyTable::find(SubtleFamily family) {
  for (const SubtleFamilyInfo &info : kFamilies) {
    if (info.family == family) {
      return info;
    }
  }
  llvm_unreachable("subtle family missing from the table");
}

}
