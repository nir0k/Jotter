#include "app/SingleInstance.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QStandardPaths>

namespace {

constexpr int TimeoutMs = 1000;

/// The longest message a launch sends: its command line and activation token on one line.
constexpr qint64 MaxMessageBytes = qint64{1} << 20;

/// The socket lives in the user's runtime directory, so it is private to the session.
QString socketPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) +
           QStringLiteral("/io.github.nir0k.Jotter.socket");
}

} // namespace

SingleInstance::SingleInstance(QObject *parent)
    : QObject(parent)
{}

bool SingleInstance::forward(const QStringList &arguments)
{
    QLocalSocket socket;
    socket.connectToServer(socketPath());
    if (!socket.waitForConnected(TimeoutMs)) {
        return false;
    }
    const QJsonObject message{
        {QStringLiteral("arguments"), QJsonArray::fromStringList(arguments)},
        {QStringLiteral("activationToken"), QString::fromUtf8(qgetenv("XDG_ACTIVATION_TOKEN"))},
    };
    socket.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
    return socket.waitForBytesWritten(TimeoutMs);
}

bool SingleInstance::listen()
{
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server.listen(socketPath())) {
        // Nobody answered forward(), so a socket file that is still there is stale.
        QLocalServer::removeServer(socketPath());
        if (!m_server.listen(socketPath())) {
            qWarning("Cannot listen on %s: %s", qPrintable(socketPath()),
                     qPrintable(m_server.errorString()));
            return false;
        }
    }
    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        QLocalSocket *socket = m_server.nextPendingConnection();
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
            if (!socket->canReadLine()) {
                // A peer that never ends its line is not a launch; it cannot fill the memory.
                if (socket->bytesAvailable() > MaxMessageBytes) {
                    socket->abort();
                }
                return;
            }
            const QJsonObject message = QJsonDocument::fromJson(socket->readLine()).object();
            QStringList arguments;
            for (const QJsonValue value : message.value(QStringLiteral("arguments")).toArray()) {
                arguments.append(value.toString());
            }
            Q_EMIT launched(arguments,
                            message.value(QStringLiteral("activationToken")).toString().toUtf8());
            socket->disconnectFromServer();
        });
    });
    return true;
}
