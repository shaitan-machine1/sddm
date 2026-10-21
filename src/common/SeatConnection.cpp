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
#include <QDir>
#include <QFile>
#include <QRegularExpression>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace SDDM {
    SeatConnection::~SeatConnection()
    {
        close();
    }

    const char *SeatConnection::environmentVariable()
    {
        return "SEATD_FD";
    }

    bool SeatConnection::isValidSeatName(const QString &seat)
    {
        static const QRegularExpression allowed(QStringLiteral("\\A[A-Za-z0-9_-]{1,255}\\z"));
        return allowed.match(seat).hasMatch();
    }

    QString SeatConnection::socketPath(const QString &directory, const QString &seat)
    {
        if (directory.isEmpty() || !QDir::isAbsolutePath(directory))
            return QString();
        if (!isValidSeatName(seat))
            return QString();
        return QStringLiteral("%1/%2.sock").arg(QDir::cleanPath(directory), seat);
    }

    bool SeatConnection::open(const QString &socketPath)
    {
        close();

        if (socketPath.isEmpty()) {
            qCritical() << "Refusing to open an empty seatd socket path";
            return false;
        }

        const QByteArray path = QFile::encodeName(socketPath);
        struct sockaddr_un address = {};
        address.sun_family = AF_UNIX;
        if (size_t(path.size()) >= sizeof(address.sun_path)) {
            qCritical() << "seatd socket path is too long:" << socketPath;
            return false;
        }
        memcpy(address.sun_path, path.constData(), size_t(path.size()));

        int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            qCritical("Could not create a seatd socket: %s", strerror(errno));
            return false;
        }

        if (::connect(fd, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) < 0) {
            qCritical("Could not connect to %s: %s", path.constData(), strerror(errno));
            ::close(fd);
            return false;
        }

        m_fd = fd;
        return true;
    }

    void SeatConnection::close()
    {
        if (m_fd < 0)
            return;
        ::close(m_fd);
        m_fd = -1;
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
