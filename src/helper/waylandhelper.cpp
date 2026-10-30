/***************************************************************************
* Copyright (c) 2021 Aleix Pol Gonzalez <aleixpol@kde.org>
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the
* Free Software Foundation, Inc.,
* 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
***************************************************************************/

#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QTimer>

#include "Configuration.h"

#include "SeatConnection.h"
#include "waylandhelper.h"
#include "waylandsocketwatcher.h"
#include "VirtualTerminal.h"

#include <fcntl.h>
#include <unistd.h>

namespace SDDM {

class SeatCompositorProcess : public QProcess {
public:
    SeatCompositorProcess(int fd, QObject *parent) : QProcess(parent), m_fd(fd) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        setChildProcessModifier([fd] { passDescriptor(fd); });
#endif
    }
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
protected:
    void setupChildProcess() override { passDescriptor(m_fd); }
#endif
private:
    static void passDescriptor(int fd) {
        if (fd < 0) return;
        const int flags = ::fcntl(fd, F_GETFD);
        if (flags < 0 || ::fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC) < 0)
            _exit(127);
    }
    int m_fd;
};

WaylandHelper::WaylandHelper(QObject *parent)
    : WaylandHelper(parent, true)
{
}

WaylandHelper::WaylandHelper(QObject *parent, bool adoptSeatConnection)
    : QObject(parent)
    , m_environment(QProcessEnvironment::systemEnvironment())
    , m_runtimeDir(QDir::tempPath() + QStringLiteral("/sddm-wayland-XXXXXX"))
    , m_watcher(new WaylandSocketWatcher(m_runtimeDir.path(), this))
{
    if (!m_runtimeDir.isValid()) {
        qCritical() << "Cannot create private Wayland runtime:" << m_runtimeDir.errorString();
        m_seatConnectionUsable = false;
    } else {
        m_environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), m_runtimeDir.path());
    }
    const QString variable = QString::fromLatin1(SeatConnection::environmentVariable());
    const QString inherited = m_environment.value(variable);
    m_environment.remove(variable);
    if (!adoptSeatConnection || inherited.isEmpty())
        return;

    bool parsed = false;
    const int fd = inherited.toInt(&parsed);
    QString reason;
    if (!parsed) {
        reason = QStringLiteral("value is not a descriptor number");
    } else if (SeatConnection::validateInherited(fd, &reason)) {
        const int flags = ::fcntl(fd, F_GETFD);
        if (flags >= 0 && ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0) {
            m_seatFd = fd;
            return;
        }
        ::close(fd);
        reason = QStringLiteral("cannot protect inherited descriptor with CLOEXEC");
    }

    // The descriptor is only ever set by the root helper, so anything else
    // means the session cannot be trusted with the seat.
    qCritical() << "Ignoring the inherited seat connection:" << reason;
    m_environment.remove(variable);
    m_seatConnectionUsable = false;
}

WaylandHelper::~WaylandHelper()
{
    stop();
    releaseSeatConnection();
}

bool WaylandHelper::startCompositor(const QString &cmd)
{
    if (!m_seatConnectionUsable)
        return false;

    m_watcher->start();
    const bool started = startProcess(cmd, &m_serverProcess);

    // Only the compositor lineage keeps the seat connection; the greeter must
    // inherit neither the descriptor nor its name.
    releaseSeatConnection();
    return started;
}

void WaylandHelper::releaseSeatConnection()
{
    m_environment.remove(QString::fromLatin1(SeatConnection::environmentVariable()));
    if (m_seatFd < 0)
        return;
    ::close(m_seatFd);
    m_seatFd = -1;
}

void stopProcess(QProcess *&process)
{
    if (process && process->state() != QProcess::NotRunning) {
        qInfo() << "Stopping..." << process->program();
        process->terminate();
        if (!process->waitForFinished(5000)) {
            process->kill();
            process->waitForFinished(25000);
        }
        process->deleteLater();
        process = nullptr;
    }
}

void WaylandHelper::stop()
{
    m_watcher->stop();
    stopProcess(m_greeterProcess);
    stopProcess(m_serverProcess);
}

bool WaylandHelper::startProcess(const QString &cmd, QProcess **p)
{
    auto *process = new SeatCompositorProcess(m_seatFd, this);
    auto environment = m_environment;
    if (m_seatFd >= 0)
        environment.insert(QString::fromLatin1(SeatConnection::environmentVariable()), QString::number(m_seatFd));
    process->setProcessEnvironment(environment);
    process->setInputChannelMode(QProcess::ForwardedInputChannel);
    connect(process, &QProcess::readyReadStandardError, this, [process] {
        qWarning() << process->readAllStandardError();
    });
    connect(process, &QProcess::readyReadStandardOutput, this, [process] {
        qInfo() << process->readAllStandardOutput();
    });
    qDebug() << "Starting Wayland process" << cmd << m_environment.value(QStringLiteral("USER"));
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            process, [](int exitCode, QProcess::ExitStatus exitStatus) {
        qDebug() << "wayland compositor finished" << exitCode << exitStatus;
        // Normal compositor exit also ends the greeter lineage. Queue this
        // so a fast exit before app.exec() is not lost.
        QTimer::singleShot(0, QCoreApplication::instance(), &QCoreApplication::quit);
    });

    auto args = QProcess::splitCommand(cmd);
    if (args.isEmpty())
        return false;
    const auto program = args.takeFirst();
    process->start(program, args);
    if (!process->waitForStarted(10000)) {
        qWarning("Failed to start \"%s\": %s",
                 qPrintable(cmd),
                 qPrintable(process->errorString()));
        return false;
    }

    if (p)
        *p = process;

    qDebug() << "started succesfully" << cmd;
    return true;
}

void WaylandHelper::startGreeter(const QString &cmd)
{
    auto args = QProcess::splitCommand(cmd);

    m_greeterProcess = new QProcess(this);
    m_greeterProcess->setProgram(args.takeFirst());
    m_greeterProcess->setArguments(args);
    connect(m_greeterProcess, &QProcess::readyReadStandardError, this, [this] {
        qWarning() << m_greeterProcess->readAllStandardError();
    });
    connect(m_greeterProcess, &QProcess::readyReadStandardOutput, this, [this] {
        qInfo() << m_greeterProcess->readAllStandardOutput();
    });
    connect(m_greeterProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            m_greeterProcess, [](int exitCode, QProcess::ExitStatus exitStatus) {
        qDebug() << "wayland greeter finished" << exitCode << exitStatus;
        QCoreApplication::instance()->quit();
    });
    if (m_watcher->status() == WaylandSocketWatcher::Started) {
        m_environment.insert(QStringLiteral("WAYLAND_DISPLAY"), m_watcher->socketName());
        m_greeterProcess->setProcessEnvironment(m_environment);
        m_greeterProcess->start();
    } else if (m_watcher->status() == WaylandSocketWatcher::Failed) {
        Q_EMIT failed();
    } else {
        connect(m_watcher, &WaylandSocketWatcher::failed, this, &WaylandHelper::failed);
        connect(m_watcher, &WaylandSocketWatcher::started, this, [this] {
            m_watcher->stop();
            m_environment.insert(QStringLiteral("WAYLAND_DISPLAY"), m_watcher->socketName());
            m_greeterProcess->setProcessEnvironment(m_environment);
            m_greeterProcess->start();
        });
    }
}

} // namespace SDDM
