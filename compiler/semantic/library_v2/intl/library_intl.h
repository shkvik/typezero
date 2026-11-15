#pragma once

#include "compiler/program/library/date/date_format_option_table/date_format_option_table.h"
#include "compiler/program/library/intl/intl_service_table/intl_service_table.h"
#include "compiler/program/library/intl/intl_shape_table/intl_shape_table.h"
#include "compiler/program/types/types.h"
#include <vector>

namespace tz {

class SemanticState;

class LibraryIntl {
public:
  explicit LibraryIntl(SemanticState &owner);

  const Type *dateOptionsType() const;
  const Type *segmentDataType() const;
  const Type *dateFormatPartType() const;
  const Type *shapeType(IntlShape shape) const;
  const Type *resolvedDateOptionsType() const;
  const Type *intlTypeNamed(llvm::StringRef name) const;
  const Type *resolvedIntlOptionsType(IntlKind kind) const;

private:
  const Type *optionType(const IntlOption &option) const;
  const Type *shapeValueType(IntlValueType type) const;
  const Type *numberOptionType(IntlOptionKey key) const;
  const Type *serviceTypeNamed(llvm::StringRef name) const;
  const Type *listedOptionType(const IntlOption &option) const;
  const Type *serviceOptionsType(const IntlService &service) const;
  const Type *dateOptionValueType(const DateFormatOptionInfo &option) const;


  std::vector<int64_t> digitRange(IntlOptionKey key) const;

  SemanticState &state;
};

}
