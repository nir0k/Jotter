#pragma once

#include <QLocalServer>
#include <QObject>
#include <QStringList>

/// Keeps one Jotter per user session: a second launch hands its command line to the running
/// instance through a local socket and exits.
class SingleInstance : public QObject
{
    Q_OBJECT

public:
    explicit SingleInstance(QObject *parent = nullptr);

    /// Sends @p arguments to a running instance.
    ///
    /// @param arguments The command line of this launch.
    /// @return true when an instance received them and this process should exit.
    bool forward(const QStringList &arguments);

    /// Starts accepting launches; a socket left by a crashed instance is replaced.
    ///
    /// @return false when the socket cannot be created; Jotter then runs without the guard.
    bool listen();

Q_SIGNALS:
    /// Emitted when another launch forwarded its command line and its window activation token.
    void launched(const QStringList &arguments, const QByteArray &activationToken);

private:
    QLocalServer m_server;
};
