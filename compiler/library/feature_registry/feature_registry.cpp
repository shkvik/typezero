#include "compiler/library/feature_registry/feature_registry.h"
#include "compiler/library/tz_modules/tz_modules_feature/tz_modules_feature.h"
#include "compiler/library/builtin_modules/builtin_modules_feature/builtin_modules_feature.h"
#include "compiler/library/result/result_feature/result_feature.h"
#include "compiler/library/json/json_feature/json_feature.h"
#include "compiler/library/reflect/reflect_feature/reflect_feature.h"
#include "compiler/library/console/console_feature/console_feature.h"
#include "compiler/library/process/process_feature/process_feature.h"
#include "compiler/library/math/math_feature/math_feature.h"
#include "compiler/library/intl/intl_feature/intl_feature.h"
#include "compiler/library/decimal/decimal_feature/decimal_feature.h"
#include "compiler/library/regex/regex_feature/regex_feature.h"
#include "compiler/library/date/date_feature/date_feature.h"
#include "compiler/library/crypto/crypto_feature/crypto_feature.h"
#include "compiler/library/weakref/weakref_feature/weakref_feature.h"
#include "compiler/library/bytes/bytes_feature/bytes_feature.h"
#include "compiler/library/fs/fs_feature/fs_feature.h"
#include "compiler/library/child_process/child_process_feature/child_process_feature.h"
#include "compiler/library/events/events_feature/events_feature.h"
#include "compiler/library/stream/stream_feature/stream_feature.h"

namespace tz {

// The order is the order of the checks the compiler had before the topics were apart: a topic
// whose hook answers first wins, as its branch did. Most hooks look at names or types no other
// topic has, so the order matters only within a hook that several topics share on one value:
// intl before decimal for `d.toLocaleString()`, events before stream for the functions of their
// modules.
FeatureRegistry::FeatureRegistry() {
  features_.push_back(std::make_unique<TzModulesFeature>());
  features_.push_back(std::make_unique<BuiltinModulesFeature>());
  features_.push_back(std::make_unique<ResultFeature>());
  features_.push_back(std::make_unique<JsonFeature>());
  features_.push_back(std::make_unique<ReflectFeature>());
  features_.push_back(std::make_unique<ConsoleFeature>());
  features_.push_back(std::make_unique<ProcessFeature>());
  features_.push_back(std::make_unique<MathFeature>());
  features_.push_back(std::make_unique<IntlFeature>());
  features_.push_back(std::make_unique<DecimalFeature>());
  features_.push_back(std::make_unique<RegexFeature>());
  features_.push_back(std::make_unique<DateFeature>());
  features_.push_back(std::make_unique<CryptoFeature>());
  features_.push_back(std::make_unique<WeakRefFeature>());
  features_.push_back(std::make_unique<BytesFeature>());
  features_.push_back(std::make_unique<FsFeature>());
  features_.push_back(std::make_unique<ChildProcessFeature>());
  features_.push_back(std::make_unique<EventsFeature>());
  features_.push_back(std::make_unique<StreamFeature>());
  for (const std::unique_ptr<Feature> &feature : features_) {
    order_.push_back(feature.get());
  }
}

const FeatureRegistry &FeatureRegistry::standard() {
  static const FeatureRegistry registry;

  return registry;
}

const Feature *FeatureRegistry::lowering(Builtin builtin) const {
  for (const Feature *feature : order_) {
    if (feature->lowers(builtin)) {
      return feature;
    }
  }

  return nullptr;
}

} // namespace tz
