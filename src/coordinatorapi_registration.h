/****************************************************************************
 * Coordinator registration for processes without a checkpoint runtime.    *
 ****************************************************************************/

#ifndef DMTCP_COORDINATORAPI_REGISTRATION_H
#define DMTCP_COORDINATORAPI_REGISTRATION_H

#include <string>

namespace dmtcp {
namespace Registration {

inline constexpr char kEnabledEnv[] = "DMTCP_MACOS_REGISTER_ONLY";

// Connects to the coordinator and registers the current process as a worker
// that participates in lifecycle commands but cannot checkpoint.  Returns the
// connected socket, or -1 and an explanation in error.
int connectToCoordinator(const char *host,
                         int port,
                         const char *programName,
                         std::string *error);

// Handles coordinator lifecycle messages until the connection closes.
void monitorCoordinator(int coordinatorFd);

} // namespace Registration
} // namespace dmtcp

#endif // DMTCP_COORDINATORAPI_REGISTRATION_H
