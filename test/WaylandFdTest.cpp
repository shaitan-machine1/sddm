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
#include <sys/un.h>
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

    // Publish the result before the socket can wake and finish the greeter.
    if (!writeResult("TEST_COMPOSITOR_RESULT", "ok\n"))
        return 14;
    const std::string socket = environmentValue("XDG_RUNTIME_DIR") + "/wayland-0";
    const int file = open(socket.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    if (file < 0)
        return 13;
    close(file);

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

bool waitForPeer()
{
    for (int i = 0; i < 500; ++i) {
        if (containsOk(environmentValue("TEST_PEER_COMPOSITOR_RESULT"))
            && containsOk(environmentValue("TEST_COMPOSITOR_RESULT")))
            return true;
        usleep(10000);
    }
    return false;
}

int isolationCompositor()
{
    const auto tag = environmentValue("TEST_TAG");
    const auto path = environmentValue("XDG_RUNTIME_DIR") + "/wayland-" + tag;
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path))
        return 30;
    std::strcpy(address.sun_path, path.c_str());
    const int server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server < 0 || bind(server, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0
        || listen(server, 2) != 0)
        return 31;
    if (!writeResult("TEST_COMPOSITOR_RESULT", "ok\n") || !waitForPeer())
        return 32;
    for (;;) {
        const int client = accept(server, nullptr, nullptr);
        if (client < 0)
            return 33;
        if (write(client, tag.data(), 1) != 1)
            return 34;
        close(client);
    }
}

int isolationGreeter()
{
    if (!waitForPeer())
        return 40;
    const auto path = environmentValue("XDG_RUNTIME_DIR") + "/" + environmentValue("WAYLAND_DISPLAY");
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path))
        return 41;
    std::strcpy(address.sun_path, path.c_str());
    const int client = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (client < 0 || connect(client, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
        return 42;
    char actual = 0;
    if (read(client, &actual, 1) != 1 || actual != environmentValue("TEST_TAG").front())
        return 43;
    close(client);
    return writeResult("TEST_GREETER_RESULT", "ok\n") ? 0 : 44;
}

pid_t startHelper(const char *self, const char *helper, const std::string &runtime,
                  int inherited, bool clearCloexec, const std::string &compositorResult,
                  const std::string &greeterResult, const std::string &inode,
                  const std::string &tag = {})
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
        if (!tag.empty()) {
            setenv("TEST_TAG", tag.c_str(), 1);
            const auto peer = runtime + "/seat" + (tag == "0" ? "1" : "0") + ".compositor";
            setenv("TEST_PEER_COMPOSITOR_RESULT", peer.c_str(), 1);
        }
        const std::string compositorCommand = std::string(self)
            + (tag.empty() ? " compositor" : " isolation-compositor");
        const std::string greeterCommand = std::string(self)
            + (tag.empty() ? " greeter" : " isolation-greeter");
        execl(helper, helper, compositorCommand.c_str(), greeterCommand.c_str(), nullptr);
        _exit(126);
    }

    return child;
}

int waitHelper(pid_t child)
{
    int status = 0;
    if (child <= 0 || waitpid(child, &status, 0) != child)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int runHelper(const char *self, const char *helper, const std::string &runtime,
              int inherited, bool clearCloexec, const std::string &compositorResult,
              const std::string &greeterResult, const std::string &inode)
{
    return waitHelper(startHelper(self, helper, runtime, inherited, clearCloexec,
                                  compositorResult, greeterResult, inode));
}
} // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::strcmp(argv[1], "compositor") == 0)
        return compositor();
    if (argc == 2 && std::strcmp(argv[1], "greeter") == 0)
        return greeter();
    if (argc == 2 && std::strcmp(argv[1], "isolation-compositor") == 0)
        return isolationCompositor();
    if (argc == 2 && std::strcmp(argv[1], "isolation-greeter") == 0) {
        const int status = isolationGreeter();
        if (status != 0) {
            const auto detail = std::to_string(status) + ": " + environmentValue("XDG_RUNTIME_DIR")
                + "/" + environmentValue("WAYLAND_DISPLAY");
            writeResult("TEST_GREETER_RESULT", detail.c_str());
        }
        return status;
    }
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
    if (helperStatus != 0 || !containsOk(compositorResult) || !containsOk(greeterResult)) {
        std::fprintf(stderr, "single helper failed: %d; compositor=%d greeter=%d\n",
                     helperStatus, containsOk(compositorResult), containsOk(greeterResult));
        result = 5;
    }

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

    // Two helpers share a UID and initial runtime directory. Each fake server
    // reports its identity over a real Unix socket; neither greeter may attach
    // to the other helper's compositor, regardless of startup ordering.
    if (result == 0) {
        const auto first = startHelper(argv[0], argv[1], runtime, pair[0], true,
            runtime + "/seat0.compositor", runtime + "/seat0.greeter", inode, "0");
        const auto second = startHelper(argv[0], argv[1], runtime, pair[0], true,
            runtime + "/seat1.compositor", runtime + "/seat1.greeter", inode, "1");
        const auto firstStatus = waitHelper(first);
        const auto secondStatus = waitHelper(second);
        if (firstStatus != 0 || secondStatus != 0
            || !containsOk(runtime + "/seat0.greeter") || !containsOk(runtime + "/seat1.greeter")) {
            std::fprintf(stderr, "concurrent greeters failed: helpers %d/%d; artifacts: %s\n",
                         firstStatus, secondStatus, runtime.c_str());
            return 7;
        }
        for (const auto *tag : {"0", "1"}) {
            unlink((runtime + "/seat" + tag + ".compositor").c_str());
            unlink((runtime + "/seat" + tag + ".greeter").c_str());
            unlink((runtime + "/wayland-" + tag).c_str());
        }
    }

    close(pair[0]);
    close(pair[1]);
    rmdir(runtime.c_str());
    return result;
}
