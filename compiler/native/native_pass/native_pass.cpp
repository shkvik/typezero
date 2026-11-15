#include "compiler/native/native_pass/native_pass.h"
#include "compiler/optimize/target/target.h"
#include "compiler/packages/graph/graph.h"
#include "compiler/pipeline/build_options/build_options.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Object/Archive.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"

namespace tz {

NativePass::NativePass(const Target &target, const BuildOptions &options)
    : target(target),
      options(options),
      platform(target.triple()),
      sources(target, options) {}

NativePass::ResultNativeLinked NativePass::run(llvm::Module &module,
                                               llvm::ArrayRef<ForeignFunction> foreign) const {
  auto link = NativeLink();
  if (this->options.packages == nullptr) {
    return ok(link);
  }
  auto entries = this->checkEntriesFound();
  if (entries.isErr()) {
    return err(this->toolErrors(entries.error()));
  }
  auto used = this->usedNames(module, foreign);
  for (const pkg::NativeEntry *entry : entries.value()) {
    auto linked = this->linkEntry(*entry, used, module, link);
    if (linked.isErr()) {
      return err(this->toolErrors(linked.error()));
    }
  }

  return ok(link);
}

NativePass::ResultEntriesFound NativePass::checkEntriesFound() const {
  auto entries = std::vector<const pkg::NativeEntry *>();
  auto seen = llvm::StringSet<>();
  for (const pkg::Package &package : this->options.packages->packages) {
    for (const pkg::NativeEntry &entry : package.natives) {
      bool isFitting = this->platform.isFitting(entry);
      bool isRepeated = isFitting && !seen.insert(entry.dir + "\n" + entry.name).second;
      if (isRepeated) {
        auto message = ErrorTextNativePass::twoFit(entry.name, this->platform.getName());

        return err(NativePassError{
            .code = NativePassErrorCode::TWO_ENTRIES_FIT,
            .message = message,
        });
      }
      if (isFitting) {
        entries.push_back(&entry);
      }
    }
  }

  return ok(entries);
}

NativePass::ResultEntryChecked NativePass::linkEntry(const pkg::NativeEntry &entry,
                                                     const llvm::StringSet<> &used,
                                                     llvm::Module &module, NativeLink &link) const {
  if (!entry.sources.empty()) {
    return this->linkSources(entry, used, module, link);
  }
  auto symbols = this->readLibrarySymbols(entry);
  if (symbols.isErr()) {
    return err(symbols.error());
  }
  if (!this->isProviding(symbols.value(), used)) {
    return ok();
  }
  auto files = this->checkEntryFiles(entry);
  if (files.isErr()) {
    return err(files.error());
  }
  auto license = this->readLicense(entry);
  if (license.isErr()) {
    return err(license.error());
  }
  this->addEntry(entry, license.value(), link);

  return ok();
}

NativePass::ResultEntryChecked NativePass::linkSources(const pkg::NativeEntry &entry,
                                                       const llvm::StringSet<> &used,
                                                       llvm::Module &module,
                                                       NativeLink &link) const {
  auto built = this->sources.buildEntry(entry, module.getContext());
  if (built.isErr()) {
    return err(built.error());
  }
  if (!this->isDefining(*built.value(), used)) {
    return ok();
  }
  auto files = this->checkEntryFiles(entry);
  if (files.isErr()) {
    return err(files.error());
  }
  auto license = this->readLicense(entry);
  if (license.isErr()) {
    return err(license.error());
  }
  auto joined = this->joinCode(entry, module, std::move(built.value()));
  if (joined.isErr()) {
    return err(joined.error());
  }
  this->addEntry(entry, license.value(), link);

  return ok();
}

NativePass::ResultEntryChecked NativePass::joinCode(const pkg::NativeEntry &entry,
                                                    llvm::Module &module,
                                                    std::unique_ptr<llvm::Module> code) const {
  code->setTargetTriple(module.getTargetTriple());
  code->setDataLayout(module.getDataLayout());
  bool isFailed = llvm::Linker::linkModules(module, std::move(code), llvm::Linker::LinkOnlyNeeded);
  if (isFailed) {
    auto message = ErrorTextNativePass::join(entry.name);

    return err(NativePassError{
        .code = NativePassErrorCode::JOIN_FAILED,
        .message = message,
    });
  }

  return ok();
}

NativePass::ResultSymbolsFound NativePass::readLibrarySymbols(const pkg::NativeEntry &entry) const {
  auto symbols = llvm::StringSet<>();
  for (const std::string &library : entry.libraries) {
    auto archive = this->readArchiveSymbols(entry, library);
    if (archive.isErr()) {
      return err(archive.error());
    }
    symbols.insert(archive.value().begin(), archive.value().end());
  }

  return ok(symbols);
}

NativePass::ResultSymbolsFound NativePass::readArchiveSymbols(const pkg::NativeEntry &entry,
                                                              llvm::StringRef path) const {
  auto buffer = llvm::MemoryBuffer::getFile(path);
  if (!buffer) {
    auto message = ErrorTextNativePass::missing(entry.name, "the library", path);

    return err(NativePassError{
        .code = NativePassErrorCode::MISSING_FILE,
        .message = message,
    });
  }
  auto archive = llvm::object::Archive::create((*buffer)->getMemBufferRef());
  if (!archive) {
    auto why = llvm::toString(archive.takeError());
    auto message = ErrorTextNativePass::unreadable(entry.name, path, why);

    return err(NativePassError{
        .code = NativePassErrorCode::UNREADABLE_LIBRARY,
        .message = message,
    });
  }
  auto symbols = llvm::StringSet<>();
  for (const llvm::object::Archive::Symbol &symbol : (*archive)->symbols()) {
    symbols.insert(symbol.getName());
  }

  return ok(symbols);
}

NativePass::ResultEntryChecked NativePass::checkEntryFiles(const pkg::NativeEntry &entry) const {
  if (!llvm::sys::fs::exists(entry.licenseFile)) {
    auto message = ErrorTextNativePass::missing(entry.name, "the license file", entry.licenseFile);

    return err(NativePassError{
        .code = NativePassErrorCode::MISSING_FILE,
        .message = message,
    });
  }
  for (const std::string &dll : entry.dlls) {
    if (!llvm::sys::fs::exists(dll)) {
      auto message = ErrorTextNativePass::missing(entry.name, "the DLL", dll);

      return err(NativePassError{
          .code = NativePassErrorCode::MISSING_FILE,
          .message = message,
      });
    }
  }

  return ok();
}

NativePass::ResultLicenseFound NativePass::readLicense(const pkg::NativeEntry &entry) const {
  auto text = llvm::MemoryBuffer::getFile(entry.licenseFile);
  if (!text) {
    auto message = ErrorTextNativePass::missing(entry.name, "the license file", entry.licenseFile);

    return err(NativePassError{
        .code = NativePassErrorCode::MISSING_FILE,
        .message = message,
    });
  }

  return ok(NativeLicense{
      .name = entry.name,
      .license = entry.license,
      .text = (*text)->getBuffer().str(),
  });
}

llvm::StringSet<> NativePass::usedNames(const llvm::Module &module,
                                        llvm::ArrayRef<ForeignFunction> foreign) const {
  auto used = llvm::StringSet<>();
  for (const ForeignFunction &function : foreign) {
    const llvm::Function *declared = module.getFunction(function.name);
    if (declared != nullptr && declared->isDeclaration()) {
      used.insert(function.name);
    }
  }

  return used;
}

bool NativePass::isDefining(const llvm::Module &code, const llvm::StringSet<> &used) const {
  for (const auto &name : used) {
    const llvm::Function *defined = code.getFunction(name.getKey());
    if (defined != nullptr && !defined->isDeclaration()) {
      return true;
    }
  }

  return false;
}

bool NativePass::isProviding(const llvm::StringSet<> &symbols,
                             const llvm::StringSet<> &used) const {
  for (const auto &name : used) {
    if (symbols.contains(name.getKey())) {
      return true;
    }
  }

  return false;
}

void NativePass::addEntry(const pkg::NativeEntry &entry, NativeLicense license,
                          NativeLink &link) const {
  link.inputs.insert(link.inputs.end(), entry.libraries.begin(), entry.libraries.end());
  for (const std::string &system : entry.systemLibraries) {
    link.inputs.push_back("/defaultlib:" + system);
  }
  link.dlls.insert(link.dlls.end(), entry.dlls.begin(), entry.dlls.end());
  link.licenses.push_back(std::move(license));
}

std::vector<Diagnostic> NativePass::toolErrors(const NativePassError &error) const {
  return {Diagnostic{.form = Diagnostic::Form::Tool, .message = error.message}};
}

}
