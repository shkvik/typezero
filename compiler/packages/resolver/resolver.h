#pragma once

#include "compiler/packages/dependency/dependency.h"
#include "compiler/packages/jsr_versions/jsr_versions.h"
#include "compiler/packages/lock/lock.h"
#include "compiler/packages/locked/locked.h"
#include "compiler/packages/manifest/manifest.h"
#include "compiler/packages/prepare_options/prepare_options.h"
#include "compiler/packages/requirement/requirement.h"
#include <map>
#include <string>
#include <vector>

namespace tz::pkg {

struct JsrVersion;

class Resolver {
public:
  Resolver(const Manifest &root, const Lock &old, const PrepareOptions &options)
      : root_(root),
        old(old),
        options(options) {}

  bool run(Lock &out);
  std::map<std::string, Manifest> locals;

private:
  static std::vector<std::string> jsrDependencies(const std::string &dir, const JsrVersion &meta);
  bool collect(std::map<std::string, std::vector<Requirement>> &reqs);
  bool updating(const std::string &id) const {
    return this->options.update && (this->options.only.empty() ||
                                    std::find(this->options.only.begin(), this->options.only.end(),
                                              id) != this->options.only.end());
  }
  bool fits(const Locked &locked, const std::vector<Requirement> &reqs) const;
  bool choose(const std::string &id, const std::vector<Requirement> &reqs, Locked &out);
  bool chooseJsr(const std::string &id, const std::vector<Requirement> &reqs, Locked &out);
  bool chooseGit(const std::string &id, const std::vector<Requirement> &reqs, Locked &out);
  void reportSpec(const std::string &by, const Dependency *dep, const std::string &message);

  const Manifest &root_;
  const Lock &old;
  const PrepareOptions &options;
  std::map<std::string, Locked> chosen;
  std::map<std::string, JsrVersions> versions;
};

}
