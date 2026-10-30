#include "SeatConnection.h"

#include <QtTest>
#include <QDBusContext>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QDir>
#include <QSocketNotifier>
#include <QProcess>
#include <cstdio>
#include <fcntl.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace SDDM;
namespace {
const QString service = QStringLiteral("org.decoupling.multiseatd1");
const QString path = QStringLiteral("/org/decoupling/multiseatd1");
const QString interface = QStringLiteral("org.decoupling.multiseatd1.Manager");
}

class FakeClientManager : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.decoupling.multiseatd1.Manager")
public:
    QStringList events;
    QHash<QString, int> peers;
    QHash<QString, QSocketNotifier *> notifiers;
    uint next = 0;
    ~FakeClientManager() override { for (int fd : peers) ::close(fd); }
public Q_SLOTS:
    QString CreateClient(const QString &seat, uint uid, QDBusUnixFileDescriptor &connection) {
        if (seat == QLatin1String("seat-fail")) {
            sendErrorReply(QDBusError::Failed, QStringLiteral("injected failure"));
            return {};
        }
        const QString id = seat == QLatin1String("seat-bad") ? QStringLiteral("invalid")
            : QStringLiteral("01900000-0000-7000-8000-%1").arg(++next, 12, 16, QLatin1Char('0'));
        int pair[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) < 0) return {};
        peers.insert(id, pair[0]);
        connection.setFileDescriptor(pair[1]);
        ::close(pair[1]);
        auto *notifier = new QSocketNotifier(pair[0], QSocketNotifier::Read, this);
        notifiers.insert(id, notifier);
        connect(notifier, &QSocketNotifier::activated, this, [this, id, notifier] {
            char byte;
            const int fd = peers.value(id, -1);
            if (::read(fd, &byte, 1) == 1) ::send(fd, &byte, 1, MSG_NOSIGNAL);
            else notifier->setEnabled(false);
        });
        events.append(QStringLiteral("create:%1:%2:%3").arg(seat).arg(uid).arg(id));
        return id;
    }
    void RevokeClient(const QString &id) {
        events.append(QStringLiteral("revoke:%1").arg(id));
        if (peers.contains(id)) {
            delete notifiers.take(id);
            ::close(peers.take(id));
        }
    }
    QStringList Audit() const { return events; }
    void Reset() { events.clear(); }
};

class SeatConnectionTest : public QObject {
    Q_OBJECT
    QProcess server;
    QStringList audit() {
        QDBusInterface proxy(service, path, interface, QDBusConnection::sessionBus());
        QDBusReply<QStringList> reply = proxy.call(QStringLiteral("Audit"));
        return reply.value();
    }
    int revocations() { return audit().filter(QRegularExpression(QStringLiteral("^revoke:"))).size(); }
private Q_SLOTS:
    void initTestCase() {
        server.start(QCoreApplication::applicationFilePath(), { QStringLiteral("--fake-manager") });
        QVERIFY(server.waitForStarted());
        QVERIFY(server.waitForReadyRead(5000));
        QCOMPARE(server.readAllStandardOutput().trimmed(), QByteArray("ready"));
    }
    void cleanupTestCase() { server.terminate(); QVERIFY(server.waitForFinished()); }
    void init() {
        QDBusInterface proxy(service, path, interface, QDBusConnection::sessionBus());
        QVERIFY(proxy.call(QStringLiteral("Reset")).type() != QDBusMessage::ErrorMessage);
    }
    void defaultConstructionDoesNotStartDBusBeforePam() {
        QProcess probe;
        auto environment = QProcessEnvironment::systemEnvironment();
        // A regressed constructor must never contact the live system bus.
        environment.insert(QStringLiteral("DBUS_SYSTEM_BUS_ADDRESS"),
                           environment.value(QStringLiteral("DBUS_SESSION_BUS_ADDRESS")));
        probe.setProcessEnvironment(environment);
        probe.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--construction-probe")});
        QVERIFY(probe.waitForStarted());
        QVERIFY(probe.waitForFinished(5000));
        if (probe.exitCode() == 77)
            QSKIP("thread-count probe needs Linux procfs");
        QCOMPARE(probe.exitStatus(), QProcess::NormalExit);
        QVERIFY2(probe.exitCode() == 0, probe.readAllStandardError().constData());
    }
    void validatesSeatNames() {
        for (const auto &seat : {QStringLiteral("seat0"), QStringLiteral("seat-usb_1")})
            QVERIFY(SeatConnection::isValidSeatName(seat));
        for (const auto &seat : {QString(), QStringLiteral("other"), QStringLiteral("seat0/../root"), QStringLiteral("seat 0"), QStringLiteral("seat0\n")})
            QVERIFY(!SeatConnection::isValidSeatName(seat));
    }
    void resolvesUidAndClosesParentCopyWithoutLosingCleanupOwnership() {
        SeatConnection connection(QDBusConnection::sessionBus());
        const auto *user = ::getpwuid(::getuid());
        QVERIFY(user);
        QVERIFY(connection.createForUser(QStringLiteral("seat0"), QString::fromLocal8Bit(user->pw_name)));
        QVERIFY(audit()[0].startsWith(QStringLiteral("create:seat0:%1:").arg(::getuid())));
        const int fd = connection.fd();
        QCOMPARE(::fcntl(fd, F_GETFD) & FD_CLOEXEC, FD_CLOEXEC);
        char byte = 'x';
        QCOMPARE(::write(fd, &byte, 1), 1);
        QCOMPARE(::read(fd, &byte, 1), 1);
        QCOMPARE(byte, 'x');
        connection.closeFd();
        QCOMPARE(::fcntl(fd, F_GETFD), -1);
        QVERIFY(!connection.clientId().isEmpty());
        QCOMPARE(revocations(), 0);
        QVERIFY(connection.revoke());
        QVERIFY(connection.revoke());
        QCOMPARE(revocations(), 1);
    }
    void onlySelectedExecReceivesExactlyOneDescriptor() {
        SeatConnection connection(QDBusConnection::sessionBus());
        QVERIFY(connection.create(QStringLiteral("seat0"), 1234));
        const int fd = connection.fd();
        struct stat info = {};
        QCOMPARE(::fstat(fd, &info), 0);
        for (bool intended : {false, true}) {
            const pid_t child = ::fork();
            QVERIFY(child >= 0);
            if (child == 0) {
                if (intended && !connection.prepareForChild()) _exit(125);
                const QByteArray descriptor = QByteArray::number(fd);
                const QByteArray inode = QByteArray::number(info.st_ino);
                const QByteArray executable = QFile::encodeName(QCoreApplication::applicationFilePath());
                ::execl(executable.constData(), executable.constData(), "--fd-probe", descriptor.constData(), inode.constData(), intended ? "yes" : "no", nullptr);
                _exit(126);
            }
            int status = 0;
            QCOMPARE(::waitpid(child, &status, 0), child);
            QVERIFY(WIFEXITED(status));
            QCOMPARE(WEXITSTATUS(status), 0);
            QCOMPARE(::fcntl(fd, F_GETFD) & FD_CLOEXEC, FD_CLOEXEC);
        }
    }
    void moveOwnershipAndReplacementEachRevokeExactlyOnce() {
        {
            SeatConnection first(QDBusConnection::sessionBus());
            QVERIFY(first.create(QStringLiteral("seat0"), 1000));
            SeatConnection second(std::move(first));
            QVERIFY(!first.isOpen()); QVERIFY(first.clientId().isEmpty());
            QVERIFY(second.create(QStringLiteral("seat1"), 1001));
            QCOMPARE(revocations(), 1);
            SeatConnection third(QDBusConnection::sessionBus());
            third = std::move(second);
            QVERIFY(!second.isOpen());
            QVERIFY(third.isOpen());
        }
        QCOMPARE(revocations(), 2);
    }
    void cleanupPaths_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("normal-exit") << QStringLiteral("normal");
        QTest::newRow("startup-failure") << QStringLiteral("failure");
        QTest::newRow("forced-stop") << QStringLiteral("forced");
        QTest::newRow("helper-teardown") << QStringLiteral("teardown");
    }
    void cleanupPaths() {
        QFETCH(QString, mode);
        int fd = -1;
        {
            SeatConnection connection(QDBusConnection::sessionBus());
            QVERIFY(connection.create(QStringLiteral("seat0"), 1000));
            fd = connection.fd();
            if (mode != QLatin1String("teardown")) {
                QProcess child;
                child.start(mode == QLatin1String("failure") ? QStringLiteral("/no/such/compositor") : QStringLiteral("/bin/sleep"),
                            {mode == QLatin1String("forced") ? QStringLiteral("30") : QStringLiteral("0")});
                connection.closeFd();
                const bool started = child.waitForStarted();
                if (mode == QLatin1String("failure")) QVERIFY(!started);
                else {
                    QVERIFY(started);
                    if (mode == QLatin1String("forced")) child.kill();
                    QVERIFY(child.waitForFinished());
                }
                QVERIFY(connection.revoke());
            }
        }
        QCOMPARE(::fcntl(fd, F_GETFD), -1);
        QCOMPARE(revocations(), 1);
    }
    void failedAndMalformedCreationNeverLeavesAnOwnedLease() {
        SeatConnection connection(QDBusConnection::sessionBus());
        QVERIFY(!connection.create(QStringLiteral("seat-fail"), 1000));
        QVERIFY(!connection.isOpen());
        QVERIFY(!connection.create(QStringLiteral("seat-bad"), 1000));
        QVERIFY(!connection.isOpen());
        QCOMPARE(revocations(), 1);
        QVERIFY(!connection.createForUser(QStringLiteral("seat0"), QStringLiteral("no-such-sddm-test-user")));
    }
    void rejectsUnusableInheritedDescriptors() {
        QString reason;
        QVERIFY(!SeatConnection::validateInherited(-1, &reason));
        int pipe[2]; QCOMPARE(::pipe(pipe), 0);
        QVERIFY(!SeatConnection::validateInherited(pipe[0], &reason));
        ::close(pipe[0]); ::close(pipe[1]);
        const int unconnected = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        QVERIFY(!SeatConnection::validateInherited(unconnected, &reason)); ::close(unconnected);
        int datagrams[2]; QCOMPARE(::socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, datagrams), 0);
        QVERIFY(!SeatConnection::validateInherited(datagrams[0], &reason));
        ::close(datagrams[0]); ::close(datagrams[1]);
    }
};

int main(int argc, char **argv) {
    if (argc == 5 && std::strcmp(argv[1], "--fd-probe") == 0) {
        const auto inode = std::strtoull(argv[3], nullptr, 10);
        int matches = 0;
        for (int fd = 3; fd < 1024; ++fd) {
            struct stat info = {};
            if (::fstat(fd, &info) == 0 && S_ISSOCK(info.st_mode) && info.st_ino == inode) ++matches;
        }
        return matches == (std::strcmp(argv[4], "yes") == 0 ? 1 : 0) ? 0 : 1;
    }
    QCoreApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--construction-probe"))) {
        const QDir tasks(QStringLiteral("/proc/self/task"));
        if (!tasks.exists())
            return 77;
        const auto before = tasks.entryList(QDir::Dirs | QDir::NoDotAndDotDot).size();
        {
            SeatConnection connection;
            SeatConnection moved(std::move(connection));
            SeatConnection replacement;
            replacement = std::move(moved);
            if (replacement.isOpen() || !replacement.revoke())
                return 1;
        }
        if (tasks.entryList(QDir::Dirs | QDir::NoDotAndDotDot).size() != before) {
            std::fputs("default seat connection started a D-Bus thread before PAM\n", stderr);
            return 1;
        }
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--fake-manager"))) {
        FakeClientManager manager;
        auto bus = QDBusConnection::sessionBus();
        if (!bus.registerObject(path, &manager, QDBusConnection::ExportAllSlots) || !bus.registerService(service)) return 1;
        std::puts("ready"); std::fflush(stdout);
        return app.exec();
    }
    SeatConnectionTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "SeatConnectionTest.moc"
