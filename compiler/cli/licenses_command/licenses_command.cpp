#include "compiler/cli/licenses_command/licenses_command.h"
#include "compiler/cli/args/args.h"
#include "compiler/link/licenses/licenses.h"

namespace tz {

LicensesCommand::LicensesCommand(const RuntimeFiles &runtime) : runtime_(runtime) {}

int LicensesCommand::run(const Args &args) const {
  return printLicenses(args.input, runtime_.path("licenses"));
}

}
