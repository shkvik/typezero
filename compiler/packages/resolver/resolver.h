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

class Resolver {
public:
  Resolver(const Manifest &root, const Lock &old, const PrepareOptions &options)
      : root_(root), old_(old), options_(options) {}

  bool run(Lock &out);
  // The local packages reached, by id.
  std::map<std::string, Manifest> locals;

private:
  bool collect(std::map<std::string, std::vector<Requirement>> &reqs);
  bool updating(const std::string &id) const {
    return options_.update &&
           (options_.only.empty() ||
            std::find(options_.only.begin(), options_.only.end(), id) != options_.only.end());
  }
  bool fits(const Locked &locked, const std::vector<Requirement> &reqs) const;
  bool choose(const std::string &id, const std::vector<Requirement> &reqs, Locked &out);
  bool chooseJsr(const std::string &id, const std::vector<Requirement> &reqs, Locked &out);
  bool chooseGit(const std::string &id, const std::vector<Requirement> &reqs, Locked &out);
  // Reports a dependency that is written wrongly, in the file that writes it.
  void reportSpec(const std::string &by, const Dependency *dep, const std::string &message);

  const Manifest &root_;
  const Lock &old_;
  const PrepareOptions &options_;
  std::map<std::string, Locked> chosen_;
  std::map<std::string, JsrVersions> versions_;
};

} // namespace tz::pkg
