#include "compiler/link/licenses/licenses.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <openssl/opensslv.h>

namespace tz {

namespace {

constexpr llvm::StringLiteral kSection = ".tzlic";
constexpr llvm::StringLiteral kHeader = "tz-licenses\n";

struct Project {
  llvm::StringLiteral id;
  llvm::StringLiteral title;
  llvm::StringLiteral license;
  llvm::StringLiteral file;
};

const Project kProjects[] = {
    {"openssl", "OpenSSL " OPENSSL_VERSION_STR ": the cryptography of node:crypto and TLS",
     "Apache License 2.0", "OpenSSL.txt"},
    {"node", "Node.js: the runtime of tz follows parts of it, such as console.log and node:path",
     "MIT", "Node.js.txt"},
    {"v8", "V8: the runtime of tz follows its parser of dates", "BSD-3-Clause", "V8.txt"},
    {"whatwg", "WHATWG Encoding Standard: the indexes of the multi-byte encodings of TextDecoder",
     "BSD-3-Clause", "WHATWG.txt"},
};

}

CryptoUse cryptoUse(const llvm::Module &module) {
  CryptoUse use;
  auto note = [&](llvm::StringRef name) {
    if (name.starts_with("tz_tls_")) {
      use.any = use.full = use.tls = true;
      return;
    }
    if (!name.starts_with("tz_crypto_") && !name.starts_with("tz_openssl_")) {
      return;
    }
    use.any = true;
    use.full |= isFullCryptoFunction(name);
  };
  for (const llvm::Function &fn : module) {
    if (fn.isDeclaration() && !fn.use_empty()) {
      note(fn.getName());
    }
  }
  for (const llvm::GlobalVariable &var : module.globals()) {
    if (var.isDeclaration()) {
      note(var.getName());
    }
  }

  return use;
}

bool usesEncodingIndexes(const llvm::Module &module) {
  for (const llvm::GlobalVariable &var : module.globals()) {
    if (var.getName().starts_with("tz_cjk_encoding_")) {
      return true;
    }
  }

  return false;
}

void addLicenseManifest(llvm::Module &module, const Embedded &embedded) {
  std::string text = kHeader.str();
  if (embedded.openssl) {
    text += "openssl\n";
  }
  text += "node\nv8\n";
  if (embedded.whatwg) {
    text += "whatwg\n";
  }
  llvm::Constant *data = llvm::ConstantDataArray::getString(module.getContext(), text);
  auto *manifest = new llvm::GlobalVariable(module, data->getType(), true,
                                            llvm::GlobalValue::PrivateLinkage, data, "tz.licenses");
  manifest->setSection(kSection);
  manifest->setAlignment(llvm::Align(1));
}

int printLicenses(llvm::StringRef exe, llvm::StringRef licensesDir) {
  llvm::Expected<llvm::object::OwningBinary<llvm::object::Binary>> binary =
      llvm::object::createBinary(exe);
  if (!binary) {
    llvm::errs() << "tz: cannot read '" << exe << "': " << llvm::toString(binary.takeError())
                 << "\n";
    return 1;
  }
  std::string manifest;
  if (auto *object = llvm::dyn_cast<llvm::object::ObjectFile>(binary->getBinary())) {
    for (const llvm::object::SectionRef &section : object->sections()) {
      llvm::Expected<llvm::StringRef> name = section.getName();
      if (!name) {
        llvm::consumeError(name.takeError());
        continue;
      }
      if (*name != kSection) {
        continue;
      }
      if (llvm::Expected<llvm::StringRef> contents = section.getContents()) {
        manifest = contents->str();
      } else {
        llvm::consumeError(contents.takeError());
      }
    }
  }
  if (!llvm::StringRef(manifest).starts_with(kHeader)) {
    llvm::errs() << "tz: '" << exe << "' was not built by tz\n";
    return 1;
  }
  llvm::SmallVector<llvm::StringRef, 8> ids;
  llvm::StringRef(manifest).drop_front(kHeader.size()).split(ids, '\n', -1, false);
  llvm::outs() << llvm::sys::path::filename(exe)
               << " embeds code of these projects; their licenses go along with it.\n";
  for (llvm::StringRef id : ids) {
    id = id.trim(llvm::StringRef("\0 \r", 3));
    for (const Project &p : kProjects) {
      if (p.id != id) {
        continue;
      }
      llvm::outs() << "\n== " << p.title << " (" << p.license << ")\n\n";
      llvm::SmallString<256> path(licensesDir);
      llvm::sys::path::append(path, p.file);
      llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> text = llvm::MemoryBuffer::getFile(path);
      if (!text) {
        llvm::errs() << "tz: the license text is missing: " << path << "\n";
        return 1;
      }
      llvm::StringRef body = (*text)->getBuffer();
      llvm::outs() << body;
      if (!body.ends_with("\n")) {
        llvm::outs() << "\n";
      }
    }
  }
  llvm::outs() << "\nNot embedded: ICU, which dates and Intl load from Windows (icu.dll), and "
                  "the libraries of Windows itself.\n";

  return 0;
}

}
