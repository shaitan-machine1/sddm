/*
 * seatd connection handling for seat-owning compositors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include "SeatConnection.h"

#include <QDebug>
#include <QRegularExpression>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <MultiseatManager.h>
#include <utility>
#include <vector>
#include <pwd.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace SDDM {
    SeatConnection::SeatConnection(const QDBusConnection &bus) : m_bus(bus) {}

    SeatConnection::SeatConnection(SeatConnection &&other) noexcept
        : m_bus(other.m_bus), m_clientId(std::exchange(other.m_clientId, {})),
          m_needsRevoke(std::exchange(other.m_needsRevoke, false)), m_fd(std::exchange(other.m_fd, -1)) {}

    SeatConnection &SeatConnection::operator=(SeatConnection &&other) noexcept
    {
        if (this != &other) {
            revoke();
            m_bus = other.m_bus;
            m_clientId = std::exchange(other.m_clientId, {});
            m_needsRevoke = std::exchange(other.m_needsRevoke, false);
            m_fd = std::exchange(other.m_fd, -1);
        }
        return *this;
    }

    SeatConnection::~SeatConnection()
    {
        revoke();
    }

    const char *SeatConnection::environmentVariable()
    {
        return "SEATD_FD";
    }

    bool SeatConnection::isValidSeatName(const QString &seat)
    {
        static const QRegularExpression allowed(QStringLiteral("\\Aseat[A-Za-z0-9_-]{0,59}\\z"));
        return allowed.match(seat).hasMatch();
    }

    bool SeatConnection::createForUser(const QString &seat, const QString &username)
    {
        const QByteArray name = username.toLocal8Bit();
        if (name.isEmpty() || name.contains('\0'))
            return false;
        std::vector<char> buffer(16384);
        struct passwd user = {};
        struct passwd *result = nullptr;
        int error;
        while ((error = getpwnam_r(name.constData(), &user, buffer.data(), buffer.size(), &result)) == ERANGE
               && buffer.size() < 1024 * 1024)
            buffer.resize(buffer.size() * 2);
        if (error || !result) {
            qCritical() << "Could not resolve seat client UID for" << username;
            return false;
        }
        return create(seat, user.pw_uid);
    }

    bool SeatConnection::create(const QString &seat, uint uid)
    {
        if (!revoke() || !isValidSeatName(seat))
            return false;
        // UserSession exists before PAM runs. Starting Qt's D-Bus worker in
        // its constructor races PAM modules' process-wide credential changes.
        if (!m_bus)
            m_bus.emplace(QDBusConnection::systemBus());
        OrgDecouplingMultiseatd1ManagerInterface proxy(QStringLiteral("org.decoupling.multiseatd1"),
            QStringLiteral("/org/decoupling/multiseatd1"), *m_bus);
        proxy.setTimeout(2000);
        QDBusPendingReply<QString, QDBusUnixFileDescriptor> reply = proxy.CreateClient(seat, uid);
        reply.waitForFinished();
        if (reply.isError()) {
            qCritical() << "Could not create multiseatd client:" << reply.error().message();
            return false;
        }
        m_clientId = reply.argumentAt<0>();
        m_needsRevoke = !m_clientId.isEmpty();
        static const QRegularExpression uuid(QStringLiteral("\\A[0-9a-f]{8}-[0-9a-f]{4}-7[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}\\z"));
        const auto received = reply.argumentAt<1>();
        QString reason;
        if (!uuid.match(m_clientId).hasMatch() || !validateInherited(received.fileDescriptor(), &reason)) {
            qCritical() << "Invalid multiseatd client reply:" << reason;
            revoke();
            return false;
        }
        m_fd = ::fcntl(received.fileDescriptor(), F_DUPFD_CLOEXEC, 3);
        if (m_fd < 0) {
            revoke();
            return false;
        }
        return true;
    }

    bool SeatConnection::revoke()
    {
        closeFd();
        if (!m_needsRevoke)
            return true;
        OrgDecouplingMultiseatd1ManagerInterface proxy(QStringLiteral("org.decoupling.multiseatd1"),
            QStringLiteral("/org/decoupling/multiseatd1"), *m_bus);
        proxy.setTimeout(2000);
        QDBusPendingReply<> reply = proxy.RevokeClient(m_clientId);
        reply.waitForFinished();
        if (reply.isError() && !revocationIsAlreadyComplete(reply.error())) {
            qWarning() << "Could not revoke multiseatd client" << m_clientId << reply.error().message();
            return false;
        }
        m_needsRevoke = false;
        m_clientId.clear();
        return true;
    }

    void SeatConnection::closeFd()
    {
        if (m_fd < 0)
            return;
        ::close(m_fd);
        m_fd = -1;
    }

    bool SeatConnection::revocationIsAlreadyComplete(const QDBusError &error)
    {
        // Name-based checks avoid depending on Qt-version-specific ErrorType
        // enumerators. If there is no service or no connection, no lease exists.
        const QString name = error.name();
        return name == QLatin1String("org.freedesktop.DBus.Error.ServiceUnknown")
            || name == QLatin1String("org.freedesktop.DBus.Error.NameHasNoOwner")
            || name == QLatin1String("org.freedesktop.DBus.Error.Disconnected");
    }

    bool SeatConnection::prepareForChild() const
    {
        if (m_fd < 0)
            return false;

        const int flags = ::fcntl(m_fd, F_GETFD);
        if (flags < 0)
            return false;
        return ::fcntl(m_fd, F_SETFD, flags & ~FD_CLOEXEC) == 0;
    }

    bool SeatConnection::validateInherited(int fd, QString *reason)
    {
        const auto fail = [reason](const QString &message) {
            if (reason)
                *reason = message;
            return false;
        };

        if (fd < 0)
            return fail(QStringLiteral("descriptor is negative"));

        struct stat info = {};
        if (::fstat(fd, &info) < 0)
            return fail(QStringLiteral("fstat failed: %1").arg(QString::fromLocal8Bit(strerror(errno))));
        if (!S_ISSOCK(info.st_mode))
            return fail(QStringLiteral("descriptor is not a socket"));

        int type = 0;
        socklen_t length = sizeof(type);
        if (::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) < 0)
            return fail(QStringLiteral("SO_TYPE failed: %1").arg(QString::fromLocal8Bit(strerror(errno))));
        if (type != SOCK_STREAM)
            return fail(QStringLiteral("descriptor is not a stream socket"));

        struct sockaddr_un peer = {};
        socklen_t peerLength = sizeof(peer);
        if (::getpeername(fd, reinterpret_cast<struct sockaddr *>(&peer), &peerLength) < 0)
            return fail(QStringLiteral("socket is not connected"));
        if (peer.sun_family != AF_UNIX)
            return fail(QStringLiteral("socket is not a local socket"));

        // libseat verifies that the peer is the root-owned seatd process, so
        // only the structural properties are checked here.
        return true;
    }
}
