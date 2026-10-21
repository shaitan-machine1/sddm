#include "SeatConnection.h"

#include <QtTest>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace SDDM;

class SeatConnectionTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void seatNames_data()
    {
        QTest::addColumn<QString>("seat");
        QTest::addColumn<bool>("valid");

        QTest::newRow("plain") << QStringLiteral("seat0") << true;
        QTest::newRow("dashes") << QStringLiteral("seat-usb_1") << true;
        QTest::newRow("empty") << QString() << false;
        QTest::newRow("slash") << QStringLiteral("seat0/../root") << false;
        QTest::newRow("parent") << QStringLiteral("..") << false;
        QTest::newRow("dot") << QStringLiteral(".") << false;
        QTest::newRow("newline") << QStringLiteral("seat0\n") << false;
        QTest::newRow("space") << QStringLiteral("seat 0") << false;
    }

    void seatNames()
    {
        QFETCH(QString, seat);
        QFETCH(bool, valid);
        QCOMPARE(SeatConnection::isValidSeatName(seat), valid);
        QCOMPARE(!SeatConnection::socketPath(QStringLiteral("/run/multiseatd"), seat).isEmpty(), valid);
    }

    void socketPaths()
    {
        QCOMPARE(SeatConnection::socketPath(QStringLiteral("/run/multiseatd"), QStringLiteral("seat0")),
                 QStringLiteral("/run/multiseatd/seat0.sock"));
        QCOMPARE(SeatConnection::socketPath(QStringLiteral("/run/multiseatd/"), QStringLiteral("seat0")),
                 QStringLiteral("/run/multiseatd/seat0.sock"));
        QVERIFY(SeatConnection::socketPath(QStringLiteral("relative"), QStringLiteral("seat0")).isEmpty());
        QVERIFY(SeatConnection::socketPath(QString(), QStringLiteral("seat0")).isEmpty());
    }

    void opensAndClosesConnections()
    {
        QTemporaryDir runtime;
        QVERIFY(runtime.isValid());
        const QString path = SeatConnection::socketPath(runtime.path(), QStringLiteral("seat0"));
        QVERIFY(!path.isEmpty());

        SeatConnection connection;
        // No listener yet.
        QVERIFY(!connection.open(path));
        QVERIFY(!connection.isOpen());

        const int listener = listen(path);
        QVERIFY(listener >= 0);

        QVERIFY(connection.open(path));
        QVERIFY(connection.isOpen());
        QVERIFY(connection.fd() >= 0);

        const int served = ::accept(listener, nullptr, nullptr);
        QVERIFY(served >= 0);

        // Close-on-exec by default, cleared only for the intended child.
        QCOMPARE(::fcntl(connection.fd(), F_GETFD) & FD_CLOEXEC, FD_CLOEXEC);
        QVERIFY(connection.prepareForChild());
        QCOMPARE(::fcntl(connection.fd(), F_GETFD) & FD_CLOEXEC, 0);

        QString reason;
        QVERIFY2(SeatConnection::validateInherited(connection.fd(), &reason), qPrintable(reason));

        // The server sees EOF once the last owner drops the descriptor.
        connection.close();
        QVERIFY(!connection.isOpen());
        QCOMPARE(connection.fd(), -1);
        char byte = 0;
        QCOMPARE(::read(served, &byte, 1), 0);

        ::close(served);
        ::close(listener);
    }

    void reopeningReplacesTheDescriptor()
    {
        QTemporaryDir runtime;
        QVERIFY(runtime.isValid());
        const QString path = SeatConnection::socketPath(runtime.path(), QStringLiteral("seat0"));
        const int listener = listen(path);
        QVERIFY(listener >= 0);

        SeatConnection connection;
        QVERIFY(connection.open(path));
        const int first = connection.fd();
        const int firstServed = ::accept(listener, nullptr, nullptr);
        QVERIFY(firstServed >= 0);

        QVERIFY(connection.open(path));
        const int secondServed = ::accept(listener, nullptr, nullptr);
        QVERIFY(secondServed >= 0);

        char byte = 0;
        QCOMPARE(::read(firstServed, &byte, 1), 0);
        QVERIFY(connection.isOpen());
        Q_UNUSED(first)

        ::close(firstServed);
        ::close(secondServed);
        ::close(listener);
    }

    void rejectsUnusableInheritedDescriptors()
    {
        QString reason;
        QVERIFY(!SeatConnection::validateInherited(-1, &reason));
        QVERIFY(!reason.isEmpty());

        int pipes[2] = { -1, -1 };
        QCOMPARE(::pipe(pipes), 0);
        QVERIFY(!SeatConnection::validateInherited(pipes[0], &reason));
        QCOMPARE(reason, QStringLiteral("descriptor is not a socket"));
        ::close(pipes[0]);
        ::close(pipes[1]);

        const int unconnected = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        QVERIFY(unconnected >= 0);
        QVERIFY(!SeatConnection::validateInherited(unconnected, &reason));
        QCOMPARE(reason, QStringLiteral("socket is not connected"));
        ::close(unconnected);

        int datagrams[2] = { -1, -1 };
        QCOMPARE(::socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, datagrams), 0);
        QVERIFY(!SeatConnection::validateInherited(datagrams[0], &reason));
        QCOMPARE(reason, QStringLiteral("descriptor is not a stream socket"));
        ::close(datagrams[0]);
        ::close(datagrams[1]);

        QVERIFY(!SeatConnection::validateInherited(4096, &reason));
    }

private:
    static int listen(const QString &path)
    {
        const QByteArray encoded = QFile::encodeName(path);
        struct sockaddr_un address = {};
        address.sun_family = AF_UNIX;
        if (size_t(encoded.size()) >= sizeof(address.sun_path))
            return -1;
        memcpy(address.sun_path, encoded.constData(), size_t(encoded.size()));

        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0)
            return -1;
        if (::bind(fd, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) < 0
            || ::listen(fd, 8) < 0) {
            ::close(fd);
            return -1;
        }
        return fd;
    }
};

QTEST_GUILESS_MAIN(SeatConnectionTest)
#include "SeatConnectionTest.moc"
