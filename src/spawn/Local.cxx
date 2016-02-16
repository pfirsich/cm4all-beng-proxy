/*
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "Local.hxx"
#include "Direct.hxx"
#include "Registry.hxx"
#include "gerrno.h"

#include <utility>

int
LocalSpawnService::SpawnChildProcess(const char *name,
                                     PreparedChildProcess &&params,
                                     ExitListener *listener,
                                     GError **error_r)
{
    pid_t pid = ::SpawnChildProcess(std::move(params));
    if (pid < 0) {
        set_error_errno_msg2(error_r, -pid, "clone() failed");
        return pid;
    }

    registry.Add(pid, name, listener);
    return pid;
}

void
LocalSpawnService::KillChildProcess(int pid, int signo)
{
    registry.Kill(pid, signo);
}
