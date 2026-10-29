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

#ifndef SDDM_SEATCONNECTION_H
#define SDDM_SEATCONNECTION_H

#include <QString>
#include <QDBusConnection>
#include <QDBusError>

namespace SDDM {
    /*!
     * Move-only ownership of a D-Bus-created client ID and seatd endpoint.
     *
     * The descriptor is opened close-on-exec, so it is only inherited by
     * children that explicitly call prepareForChild() between fork() and
     * exec(). The owner closes its copy once the child has been started so
     * that seatd observes EOF when the compositor lineage dies. Explicit
     * revocation ownership survives closing the parent descriptor.
     */
    class SeatConnection {
    public:
        explicit SeatConnection(const QDBusConnection &bus = QDBusConnection::systemBus());
        SeatConnection(SeatConnection &&other) noexcept;
        SeatConnection &operator=(SeatConnection &&other) noexcept;
        ~SeatConnection();

        SeatConnection(const SeatConnection &) = delete;
        SeatConnection &operator=(const SeatConnection &) = delete;

        //! Root helper resolves the authenticated target user before creation.
        bool createForUser(const QString &seat, const QString &username);
        bool create(const QString &seat, uint uid);
        //! Close the parent's FD after fork without ending explicit cleanup ownership.
        void closeFd();
        //! Idempotent explicit cleanup, also attempted by the destructor.
        bool revoke();
        static bool revocationIsAlreadyComplete(const QDBusError &error);
        QString clientId() const { return m_clientId; }

        bool isOpen() const { return m_fd >= 0; }
        int fd() const { return m_fd; }

        /*!
         * Clears FD_CLOEXEC. Only call this in a forked child that is about
         * to exec the intended compositor lineage.
         */
        bool prepareForChild() const;

        static bool isValidSeatName(const QString &seat);

        /*!
         * Validates a descriptor inherited through SEATD_FD. Returns false and
         * fills reason if it is not a connected local stream socket. libseat
         * additionally verifies that the peer is the root-owned seatd.
         */
        static bool validateInherited(int fd, QString *reason);

        static const char *environmentVariable();

    private:
        QDBusConnection m_bus;
        QString m_clientId;
        bool m_needsRevoke { false };
        int m_fd { -1 };
    };
}

#endif // SDDM_SEATCONNECTION_H
