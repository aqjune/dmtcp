/****************************************************************************
 * Portable command-side subset of CoordinatorAPI.                         *
 ****************************************************************************/

#include "coordinatorapi.h"

#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "../jalib/jconvert.h"
#include "../jalib/jsocket.h"
#include "util.h"
#include "dmtcp_assert.h"

namespace dmtcp {
namespace CoordinatorAPI {

CoordinatorCmd
parseCoordinatorCmd(const char *command)
{
  if (command == nullptr) {
    return DMT_INVALID_COORDINATOR_COMMAND;
  }
  while (*command == '-') {
    ++command;
  }

  if (strcmp(command, "s") == 0 || strcmp(command, "status") == 0) {
    return DMT_STATUS;
  }
  if (strcmp(command, "l") == 0 || strcmp(command, "t") == 0 ||
      strcmp(command, "list") == 0) {
    return DMT_LIST;
  }
  if (strcmp(command, "c") == 0 || strcmp(command, "checkpoint") == 0) {
    return DMT_CHECKPOINT;
  }
  if (strcmp(command, "bc") == 0 || strcmp(command, "bcheckpoint") == 0) {
    return DMT_BLOCKING_CKPT;
  }
  if (strcmp(command, "kc") == 0 || strcmp(command, "ck") == 0 ||
      strcmp(command, "K") == 0 || strcmp(command, "Kc") == 0 ||
      strcmp(command, "kcheckpoint") == 0) {
    return DMT_KILL_AFTER_CKPT;
  }
  if (strcmp(command, "i") == 0 || strcmp(command, "interval") == 0) {
    return DMT_UPDATE_CKPT_INTERVAL;
  }
  if (strcmp(command, "k") == 0 || strcmp(command, "kill") == 0) {
    return DMT_KILL;
  }
  if (strcmp(command, "q") == 0 || strcmp(command, "quit") == 0) {
    return DMT_QUIT;
  }
  if (strcmp(command, "h") == 0 || strcmp(command, "help") == 0 ||
      strcmp(command, "?") == 0) {
    return DMT_HELP;
  }
  return DMT_INVALID_COORDINATOR_COMMAND;
}

static int
createSocketToCoordinator()
{
  const char *host = getenv(ENV_VAR_NAME_HOST);
  if (host == nullptr) {
    host = getenv("DMTCP_HOST"); // Deprecated compatibility name.
  }
  if (host == nullptr) {
    host = DEFAULT_HOST;
  }

  const char *portText = getenv(ENV_VAR_NAME_PORT);
  if (portText == nullptr) {
    portText = getenv("DMTCP_PORT"); // Deprecated compatibility name.
  }
  const int port = portText == nullptr ? DEFAULT_PORT
                                       : jalib::StringToInt(portText);

  jalib::JSocket socket = jalib::JClientSocket(host, port);
  if (socket.isValid()) {
    socket.enableNoDelay();
  }
  return socket.sockfd();
}

static bool
receiveMessage(int fd, DmtcpMessage *message, void **extraData)
{
  DmtcpMessage incoming;
  incoming.poison();
  if (Util::readAll(fd, &incoming, sizeof(incoming)) != sizeof(incoming) ||
      !incoming.isValid()) {
    return false;
  }

  if (incoming.extraBytes > 0) {
    ASSERT_NOT_NULL(extraData,
                    "coordinator response payload has no destination: "
                    "type={} bytes={}",
                    incoming.type, incoming.extraBytes);
    void *buffer = JALLOC_HELPER_MALLOC(incoming.extraBytes);
    if (Util::readAll(fd, buffer, incoming.extraBytes) !=
        static_cast<ssize_t>(incoming.extraBytes)) {
      JALLOC_HELPER_FREE(buffer);
      return false;
    }
    *extraData = buffer;
  }

  *message = incoming;
  return true;
}

char *
connectAndSendUserCommand(CoordinatorCmd command,
                          CoordinatorCmdStatus *coordCmdStatus,
                          int *numPeers,
                          int *isRunning,
                          int *ckptInterval,
                          DmtcpMessage *response)
{
  char *replyData = nullptr;
  if (response != nullptr) {
    *response = DmtcpMessage(DMT_USER_CMD_RESULT);
    response->coordCmd = command;
  }

  const int coordinatorFd = createSocketToCoordinator();
  if (coordinatorFd == -1) {
    if (coordCmdStatus != nullptr) {
      *coordCmdStatus = DMT_COORD_NOT_FOUND;
    }
    if (response != nullptr) {
      response->coordCmdStatus = DMT_COORD_NOT_FOUND;
    }
    return nullptr;
  }

  DmtcpMessage request(DMT_USER_CMD);
  request.coordCmd = command;
  if (command == DMT_UPDATE_CKPT_INTERVAL) {
    const char *interval = getenv(ENV_VAR_CKPT_INTR);
    if (interval != nullptr) {
      request.theCheckpointInterval = jalib::StringToInt(interval);
    }
  }

  ASSERT_EQ(static_cast<ssize_t>(sizeof(request)),
            Util::writeAll(coordinatorFd, &request, sizeof(request)),
            "failed to send user command to coordinator: fd={} command={}",
            coordinatorFd, coordinatorCmdName(command));

  if (command == DMT_QUIT) {
    if (coordCmdStatus != nullptr) {
      *coordCmdStatus = DMT_COORD_SUCCESS;
    }
    if (response != nullptr) {
      response->coordCmdStatus = DMT_COORD_SUCCESS;
    }
    close(coordinatorFd);
    return nullptr;
  }

  DmtcpMessage reply;
  if (!receiveMessage(coordinatorFd, &reply,
                      reinterpret_cast<void **>(&replyData))) {
    close(coordinatorFd);
    if (coordCmdStatus != nullptr) {
      *coordCmdStatus = DMT_COORD_NOT_FOUND;
    }
    if (response != nullptr) {
      response->coordCmdStatus = DMT_COORD_NOT_FOUND;
    }
    return nullptr;
  }
  close(coordinatorFd);

  reply.assertValid();
  ASSERT(reply.type == DMT_USER_CMD_RESULT,
         "unexpected coordinator user-command reply type: type={}",
         reply.type);
  reply.coordCmd = command;

  if (response != nullptr) {
    *response = reply;
  }
  if (coordCmdStatus != nullptr) {
    *coordCmdStatus = reply.coordCmdStatus;
  }
  if (numPeers != nullptr) {
    *numPeers = reply.numPeers;
  }
  if (isRunning != nullptr) {
    *isRunning = reply.isRunning;
  }
  if (ckptInterval != nullptr) {
    *ckptInterval = reply.theCheckpointInterval;
  }

  return replyData;
}

} // namespace CoordinatorAPI
} // namespace dmtcp
