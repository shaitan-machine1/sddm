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

namespace SDDM {
    /*!
     * Owns one connected seatd client socket.
     *
     * The descriptor is opened close-on-exec, so it is only inherited by
     * children that explicitly call prepareForChild() between fork() and
     * exec(). The owner closes its copy once the child has been started so
     * that seatd observes EOF when the compositor lineage dies.
     */
    class SeatConnection {
    public:
        SeatConnection() = default;
        ~SeatConnection();

        SeatConnection(const SeatConnection &) = delete;
        SeatConnection &operator=(const SeatConnection &) = delete;

        //! Connects to socketPath, replacing any previously held descriptor.
        bool open(const QString &socketPath);
        void close();

        bool isOpen() const { return m_fd >= 0; }
        int fd() const { return m_fd; }

        /*!
         * Clears FD_CLOEXEC. Only call this in a forked child that is about
         * to exec the intended compositor lineage.
         */
        bool prepareForChild() const;

        //! Seat names are used to build socket paths, so keep them strict.
        static bool isValidSeatName(const QString &seat);
        //! Empty on invalid input.
        static QString socketPath(const QString &directory, const QString &seat);

        /*!
         * Validates a descriptor inherited through SEATD_FD. Returns false and
         * fills reason if it is not a connected local stream socket. libseat
         * additionally verifies that the peer is the root-owned seatd.
         */
        static bool validateInherited(int fd, QString *reason);

        static const char *environmentVariable();

    private:
        int m_fd { -1 };
    };
}

#endif // SDDM_SEATCONNECTION_H
