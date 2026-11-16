#include "app/SyncStatus.h"

#include "core/SyncScheduler.h"

#include <QCoreApplication>
#include <QLocale>

SyncStatus syncStatus(const core::SyncScheduler &scheduler)
{
    if (!scheduler.isConnected()) {
        return {};
    }
    switch (scheduler.phase()) {
    case core::SyncScheduler::Phase::Running:
        return {.text = scheduler.total() > 0
                            ? QCoreApplication::translate("SyncStatus", "Syncing %1 of %2")
                                  .arg(scheduler.done())
                                  .arg(scheduler.total())
                            : QCoreApplication::translate("SyncStatus", "Syncing…"),
                .icon = QStringLiteral("Refresh"),
                .failed = false};
    case core::SyncScheduler::Phase::Waiting:
        return {.text = QCoreApplication::translate("SyncStatus", "Waiting for another device"),
                .icon = QStringLiteral("Time"),
                .failed = false};
    case core::SyncScheduler::Phase::Failed:
        return {.text = QCoreApplication::translate("SyncStatus", "Sync failed"),
                .icon = QStringLiteral("Warning"),
                .failed = true};
    case core::SyncScheduler::Phase::Idle:
        break;
    }
    const QDateTime last = scheduler.lastSynced();
    QString text;
    if (!last.isValid()) {
        text = QCoreApplication::translate("SyncStatus", "Not synced yet");
    } else if (const qint64 minutes = last.secsTo(QDateTime::currentDateTimeUtc()) / 60;
               minutes < 1) {
        text = QCoreApplication::translate("SyncStatus", "Synced just now");
    } else if (minutes < 60) {
        text = QCoreApplication::translate("SyncStatus", "Synced %n min ago", nullptr,
                                           static_cast<int>(minutes));
    } else if (minutes < qint64{24} * 60) {
        text = QCoreApplication::translate("SyncStatus", "Synced %n h ago", nullptr,
                                           static_cast<int>(minutes / 60));
    } else {
        text = QCoreApplication::translate("SyncStatus", "Synced %1")
                   .arg(QLocale().toString(last.toLocalTime(), QLocale::ShortFormat));
    }
    return {.text = text, .icon = QStringLiteral("Cloud"), .failed = false};
}
