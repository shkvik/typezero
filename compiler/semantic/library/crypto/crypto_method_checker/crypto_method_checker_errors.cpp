#include "compiler/semantic/library/crypto/crypto_method_checker/crypto_method_checker_errors.h"

namespace tz {

std::string ErrorTextCryptoMethod::issuer(llvm::StringRef type) {
  return "'checkIssued' takes an X509Certificate; found '" + type.str() + "'";
}

std::string ErrorTextCryptoMethod::stream(llvm::StringRef name, llvm::StringRef type,
                                          Type::Crypto kind, bool cipher) {
  const char *instead = kind == Type::Crypto::Sign ? "'update(data)' and 'sign(key)'"
                        : kind == Type::Crypto::Verify
                            ? "'update(data)' and 'verify(key, signature)'"
                        : cipher ? "'update(data)' and 'final()'"
                                 : "'update(data)' and 'digest()'";

  return "'" + name.str() + "' of " + type.str() +
         " is not supported yet: in node it is a stream too, and tz has no "
         "streams yet (docs/streams.md); use " +
         instead;
}

std::string ErrorTextCryptoMethod::noMethod(llvm::StringRef name, llvm::StringRef type,
                                            llvm::StringRef methods) {
  return "property '" + name.str() + "' does not exist on type '" + type.str() + "'; it " + "has " +
         methods.str();
}

}
