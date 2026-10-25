#pragma once

#include <cstdint>
#include <string>

namespace codestudio {
// An opaque birth identity for the calling process. Throws when the operating
// system cannot supply one. Neither function grants permission to control a PID.
std::string current_process_identity();

// Read-only observation at an instant, never a promise of continued liveness.
// Returns matching_process, not_running, identity_mismatch, or unavailable.
// Malformed identities, unsupported systems and inconclusive queries return
// unavailable. A PID alone or an old heartbeat never establishes a match.
std::string observe_process_identity(std::int64_t pid, const std::string& identity);
}
