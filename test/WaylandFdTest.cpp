/*
 * Verifies that sddm-helper-start-wayland hands the inherited seat connection
 * to the compositor only, on the descriptor number that was actually
 * inherited, and that the QML greeter receives neither the descriptor nor
 * SEATD_FD.
 */

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
// Deliberately not 3: the real descriptor number is whatever the root helper
// happened to open.
const int seatFd = 11;

std::string environmentValue(const char *name)
{
    const char *value = std::getenv(name);
    return value ? value : "";
}

bool writeResult(const char *name, const char *value)
{
    const std::string path = environmentValue(name);
    FILE *file = std::fopen(path.c_str(), "w");
    if (!file)
        return false;
    const bool written = std::fputs(value, file) >= 0;
    return std::fclose(file) == 0 && written;
}

bool isSeatDescriptor(int fd)
{
    struct stat descriptor {};
    return fstat(fd, &descriptor) == 0 && S_ISSOCK(descriptor.st_mode)
        && std::to_string(descriptor.st_ino) == environmentValue("TEST_SEAT_INODE");
}

int compositor()
{
    if (environmentValue("SEATD_FD") != std::to_string(seatFd))
        return 10;
    if (!isSeatDescriptor(seatFd))
        return 11;

    // The connection must be usable, i.e. still connected to the server.
    if (write(seatFd, "x", 1) != 1)
        return 12;

    const std::string socket = environmentValue("XDG_RUNTIME_DIR") + "/wayland-0";
    const int file = open(socket.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    if (file < 0)
        return 13;
    close(file);

    if (!writeResult("TEST_COMPOSITOR_RESULT", "ok\n"))
        return 14;

    for (;;)
        pause();
}

int greeter()
{
    if (std::getenv("SEATD_FD"))
        return 20;
    if (environmentValue("WAYLAND_DISPLAY") != "wayland-0")
        return 21;
    if (isSeatDescriptor(seatFd))
        return 22;
    return writeResult("TEST_GREETER_RESULT", "ok\n") ? 0 : 23;
}

bool containsOk(const std::string &path)
{
    FILE *file = std::fopen(path.c_str(), "r");
    if (!file)
        return false;
    char value[4] {};
    const bool ok = std::fgets(value, sizeof(value), file) && std::strcmp(value, "ok\n") == 0;
    std::fclose(file);
    return ok;
}

int runHelper(const char *self, const char *helper, const std::string &runtime,
              int inherited, bool clearCloexec, const std::string &compositorResult,
              const std::string &greeterResult, const std::string &inode)
{
    const pid_t child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        if (dup2(inherited, seatFd) < 0)
            _exit(125);
        const int flags = fcntl(seatFd, F_GETFD);
        if (flags < 0)
            _exit(125);
        if (fcntl(seatFd, F_SETFD, clearCloexec ? (flags & ~FD_CLOEXEC) : (flags | FD_CLOEXEC)) < 0)
            _exit(125);
        setenv("XDG_RUNTIME_DIR", runtime.c_str(), 1);
        setenv("SEATD_FD", std::to_string(seatFd).c_str(), 1);
        setenv("TEST_SEAT_INODE", inode.c_str(), 1);
        setenv("TEST_COMPOSITOR_RESULT", compositorResult.c_str(), 1);
        setenv("TEST_GREETER_RESULT", greeterResult.c_str(), 1);
        const std::string compositorCommand = std::string(self) + " compositor";
        const std::string greeterCommand = std::string(self) + " greeter";
        execl(helper, helper, compositorCommand.c_str(), greeterCommand.c_str(), nullptr);
        _exit(126);
    }

    int status = 0;
    if (waitpid(child, &status, 0) != child)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::strcmp(argv[1], "compositor") == 0)
        return compositor();
    if (argc == 2 && std::strcmp(argv[1], "greeter") == 0)
        return greeter();
    if (argc != 2)
        return 64;

    char runtimeTemplate[] = "/tmp/sddm-wayland-fd.XXXXXX";
    char *runtimeDirectory = mkdtemp(runtimeTemplate);
    if (!runtimeDirectory)
        return 1;
    const std::string runtime = runtimeDirectory;
    const std::string compositorResult = runtime + "/compositor.result";
    const std::string greeterResult = runtime + "/greeter.result";

    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0)
        return 2;
    struct stat descriptor {};
    if (fstat(pair[0], &descriptor) != 0)
        return 3;
    const std::string inode = std::to_string(descriptor.st_ino);

    int result = 0;
    const int helperStatus = runHelper(argv[0], argv[1], runtime, pair[0], true,
                                       compositorResult, greeterResult, inode);
    if (helperStatus != 0 || !containsOk(compositorResult) || !containsOk(greeterResult))
        result = 5;

    // The helper dropped its own copy, so the socket stays open only because
    // the compositor still holds it; nothing of the lineage survives the test.
    unlink((runtime + "/wayland-0").c_str());
    unlink(compositorResult.c_str());
    unlink(greeterResult.c_str());

    // A descriptor that was not actually inherited must be refused instead of
    // being reused by some unrelated process descriptor.
    if (result == 0) {
        const int refusedStatus = runHelper(argv[0], argv[1], runtime, pair[0], false,
                                            compositorResult, greeterResult, inode);
        if (refusedStatus == 0)
            result = 6;
        unlink((runtime + "/wayland-0").c_str());
        unlink(compositorResult.c_str());
        unlink(greeterResult.c_str());
    }

    close(pair[0]);
    close(pair[1]);
    rmdir(runtime.c_str());
    return result;
}
