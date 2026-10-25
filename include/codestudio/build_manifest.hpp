#pragma once
#include "codestudio/types.hpp"

namespace codestudio {
// Validate the current explicit manifest without rewriting its contents.
// Never infer tool commands or inject tool-specific environment values.
// WorkspaceExecution performs execution-policy validation before dispatch.
Json normalize_build_manifest(const Json& manifest);
}
