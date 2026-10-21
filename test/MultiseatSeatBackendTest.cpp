#include "MultiseatSeatBackend.h"

#include <QDBusConnection>
#include <QSignalSpy>
#include <QtTest>

namespace {
const QString serviceName = QStringLiteral("org.decoupling.multiseatd1");
const QString managerPath = QStringLiteral("/org/decoupling/multiseatd1");
}

class FakeManager : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.decoupling.multiseatd1.Manager")
public:
    QStringList seats;

public Q_SLOTS:
    QStringList ListSeats() const { return seats; }

Q_SIGNALS:
    void SeatNew(const QString &name);
    void SeatRemoved(const QString &name);
};

class MultiseatSeatBackendTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void listsSignalsAndReconcilesRestarts()
    {
        QDBusConnection bus = QDBusConnection::sessionBus();
        QVERIFY(bus.isConnected());

        FakeManager manager;
        manager.seats = { QStringLiteral("seat0") };
        QVERIFY(bus.registerObject(managerPath, &manager,
                                   QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals));
        QVERIFY(bus.registerService(serviceName));

        SDDM::MultiseatSeatBackend backend(bus);
        QSignalSpy added(&backend, &SDDM::MultiseatSeatBackend::seatAdded);
        QSignalSpy removed(&backend, &SDDM::MultiseatSeatBackend::seatRemoved);
        backend.start();

        QTRY_COMPARE(added.count(), 1);
        QCOMPARE(added.at(0).at(0).toString(), QStringLiteral("seat0"));

        // New seats arrive through signals.
        manager.seats.append(QStringLiteral("seat1"));
        Q_EMIT manager.SeatNew(QStringLiteral("seat1"));
        QTRY_COMPARE(added.count(), 2);
        QCOMPARE(added.at(1).at(0).toString(), QStringLiteral("seat1"));

        // Duplicate and unknown notifications are ignored.
        Q_EMIT manager.SeatNew(QStringLiteral("seat1"));
        Q_EMIT manager.SeatRemoved(QStringLiteral("seat9"));
        Q_EMIT manager.SeatNew(QString());
        QTest::qWait(50);
        QCOMPARE(added.count(), 2);
        QCOMPARE(removed.count(), 0);

        manager.seats.removeAll(QStringLiteral("seat1"));
        Q_EMIT manager.SeatRemoved(QStringLiteral("seat1"));
        QTRY_COMPARE(removed.count(), 1);
        QCOMPARE(removed.at(0).at(0).toString(), QStringLiteral("seat1"));

        // Losing the owner removes every known seat.
        QVERIFY(bus.unregisterService(serviceName));
        QTRY_COMPARE(removed.count(), 2);
        QCOMPARE(removed.at(1).at(0).toString(), QStringLiteral("seat0"));

        // A new owner is relisted, including seats that disappeared meanwhile.
        manager.seats = { QStringLiteral("seat2") };
        QVERIFY(bus.registerService(serviceName));
        QTRY_COMPARE(added.count(), 3);
        QCOMPARE(added.at(2).at(0).toString(), QStringLiteral("seat2"));
        QCOMPARE(removed.count(), 2);

        bus.unregisterObject(managerPath);
    }
};

QTEST_GUILESS_MAIN(MultiseatSeatBackendTest)
#include "MultiseatSeatBackendTest.moc"
