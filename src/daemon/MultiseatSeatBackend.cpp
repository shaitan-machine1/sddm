#include "MultiseatSeatBackend.h"

#include <MultiseatManager.h>

#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDebug>

namespace {
const QString serviceName = QStringLiteral("org.decoupling.multiseatd1");
const QString managerPath = QStringLiteral("/org/decoupling/multiseatd1");
}

namespace SDDM {

MultiseatSeatBackend::MultiseatSeatBackend(const QDBusConnection &bus,
                                           QObject *parent)
    : QObject(parent)
    , m_bus(bus)
    , m_proxy(new OrgDecouplingMultiseatd1ManagerInterface(serviceName,
                                                            managerPath,
                                                            bus,
                                                            this))
    , m_serviceWatcher(new QDBusServiceWatcher(serviceName,
                                                bus,
                                                QDBusServiceWatcher::WatchForOwnerChange,
                                                this))
{
    connect(m_serviceWatcher, &QDBusServiceWatcher::serviceOwnerChanged,
            this, &MultiseatSeatBackend::serviceOwnerChanged);
}

void MultiseatSeatBackend::start()
{
    // Connect typed signals before listing. D-Bus preserves ordering on this
    // connection; idempotent handlers close the subscribe/list race.
    connect(m_proxy, &OrgDecouplingMultiseatd1ManagerInterface::SeatNew,
            this, &MultiseatSeatBackend::seatNew);
    connect(m_proxy, &OrgDecouplingMultiseatd1ManagerInterface::SeatRemoved,
            this, &MultiseatSeatBackend::seatGone);
    refresh();
}

void MultiseatSeatBackend::serviceOwnerChanged(const QString &service,
                                                const QString &oldOwner,
                                                const QString &newOwner)
{
    Q_UNUSED(service)
    ++m_generation;
    if (!oldOwner.isEmpty())
        clear();
    if (!newOwner.isEmpty())
        refresh();
}

void MultiseatSeatBackend::seatNew(const QString &name)
{
    if (name.isEmpty() || m_seats.contains(name))
        return;
    m_seats.insert(name);
    Q_EMIT seatAdded(name);
}

void MultiseatSeatBackend::seatGone(const QString &name)
{
    if (m_seats.remove(name))
        Q_EMIT seatRemoved(name);
}

void MultiseatSeatBackend::refresh()
{
    const quint64 generation = m_generation;
    auto *watcher = new QDBusPendingCallWatcher(m_proxy->ListSeats(), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, generation](QDBusPendingCallWatcher *finished) {
        const QDBusPendingReply<QStringList> reply = *finished;
        finished->deleteLater();
        if (generation != m_generation)
            return;
        if (reply.isError()) {
            qWarning() << "Unable to list multiseatd seats:"
                       << reply.error().message();
            return;
        }

        const QStringList values = reply.value();
        const QSet<QString> listed(values.cbegin(), values.cend());
        for (const QString &name : listed)
            seatNew(name);
        const auto previous = m_seats;
        for (const QString &name : previous) {
            if (!listed.contains(name))
                seatGone(name);
        }
    });
}

void MultiseatSeatBackend::clear()
{
    const auto previous = m_seats;
    for (const QString &name : previous)
        seatGone(name);
}

} // namespace SDDM
