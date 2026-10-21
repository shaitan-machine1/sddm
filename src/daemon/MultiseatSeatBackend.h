#ifndef SDDM_MULTISEATSEATBACKEND_H
#define SDDM_MULTISEATSEATBACKEND_H

#include <QDBusConnection>
#include <QObject>
#include <QSet>

class OrgDecouplingMultiseatd1ManagerInterface;
class QDBusServiceWatcher;

namespace SDDM {

class MultiseatSeatBackend : public QObject
{
    Q_OBJECT
public:
    explicit MultiseatSeatBackend(const QDBusConnection &bus,
                                  QObject *parent = nullptr);
    void start();

Q_SIGNALS:
    void seatAdded(const QString &name);
    void seatRemoved(const QString &name);

private Q_SLOTS:
    void serviceOwnerChanged(const QString &service,
                             const QString &oldOwner,
                             const QString &newOwner);
    void seatNew(const QString &name);
    void seatGone(const QString &name);

private:
    void refresh();
    void clear();

    QDBusConnection m_bus;
    OrgDecouplingMultiseatd1ManagerInterface *m_proxy;
    QDBusServiceWatcher *m_serviceWatcher;
    QSet<QString> m_seats;
    quint64 m_generation { 0 };
};

} // namespace SDDM

#endif
