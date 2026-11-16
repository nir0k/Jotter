#include "core/Error.h"

#include "core/Attachments.h"

#include <QCoreApplication>
#include <QDir>

namespace core {

QString describe(const Error &error)
{
    const QString path = QDir::toNativeSeparators(error.path);
    switch (error.code) {
    case ErrorCode::Deferred:
        return QCoreApplication::translate(
            "core::Error",
            "Some files changed during synchronisation. Retry to send the remaining changes.");
    case ErrorCode::Canceled:
        return QCoreApplication::translate("core::Error", "Synchronisation was stopped.");
    case ErrorCode::Io:
        return QCoreApplication::translate("core::Error", "Could not read or write %1.").arg(path);
    case ErrorCode::NotFound:
        return QCoreApplication::translate("core::Error", "%1 was not found.").arg(path);
    case ErrorCode::Malformed:
        return QCoreApplication::translate("core::Error", "%1 is damaged or not a Jotter file.")
            .arg(path);
    case ErrorCode::NewerFormat:
        return QCoreApplication::translate("core::Error",
                                           "%1 was made by a newer version of Jotter.")
            .arg(path);
    case ErrorCode::NotEmpty:
        return QCoreApplication::translate("core::Error",
                                           "%1 already contains files. Choose an empty folder.")
            .arg(path);
    case ErrorCode::ReadOnly:
        return QCoreApplication::translate("core::Error", "The library is open read-only.");
    case ErrorCode::TooLarge:
        return QCoreApplication::translate("core::Error",
                                           "%1 is too large: a file can have up to %2 MB.")
            .arg(path)
            .arg(MaxFileBytes / (qint64{1024} * 1024));
    case ErrorCode::Refused:
        return QCoreApplication::translate("core::Error", "%1 cannot be added to a note.")
            .arg(path);
    case ErrorCode::Locked:
        return QCoreApplication::translate("core::Error",
                                           "Another device is synchronising %1. Jotter will try "
                                           "again shortly.")
            .arg(path);
    case ErrorCode::OtherLibrary:
        return QCoreApplication::translate("core::Error",
                                           "%1 holds another library. Choose another folder.")
            .arg(path);
    case ErrorCode::Network:
        return QCoreApplication::translate("core::Error", "Could not connect to %1.").arg(path);
    case ErrorCode::SignIn:
        return QCoreApplication::translate("core::Error",
                                           "Could not sign in to %1. Check the user name and "
                                           "the sign-in method.")
            .arg(path);
    case ErrorCode::HostKey:
        return QCoreApplication::translate("core::Error",
                                           "The identity of %1 is not confirmed. Check it in "
                                           "Settings → Sync.")
            .arg(path);
    }
    return {};
}

} // namespace core
