#pragma once

#include "core/Files.h"
#include "core/RemoteStorage.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cstdio>

/// A sync storage in a local folder, standing in for a remote one in tests. Its modification
/// times follow the kernel's coarse clock, so a write may keep the time of the one before.
class LocalStorage : public core::RemoteStorage
{
public:
    explicit LocalStorage(QString root)
        : m_root(std::move(root))
    {
        QDir().mkpath(m_root);
    }

    QString location() const override { return m_root; }

    int timePrecisionMs() const override { return 20; }

    core::Result<QList<core::RemoteEntry>> list(const QString &path) override
    {
        const QDir dir(absolute(path));
        if (!dir.exists()) {
            return core::fail(core::ErrorCode::NotFound, dir.path());
        }
        QList<core::RemoteEntry> entries;
        for (const QFileInfo &info :
             dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)) {
            if (!info.isSymLink()) {
                entries.append(entry(info));
            }
        }
        return entries;
    }

    core::Result<void> readTo(const QString &path, QIODevice &target) override
    {
        auto data = core::readFile(absolute(path));
        if (!data) {
            return std::unexpected(data.error());
        }
        if (target.write(*data) != data->size()) {
            return core::fail(core::ErrorCode::Io, path);
        }
        return {};
    }

    core::Result<core::RemoteEntry> writeFrom(const QString &path, QIODevice &source) override
    {
        const QString target = absolute(path);
        const QString temp = QFileInfo(target).absolutePath() + u"/.jotter-tmp-" + core::newId();
        QFile file(temp);
        const QByteArray data = source.readAll();
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) {
            return core::fail(core::ErrorCode::Io, temp);
        }
        file.close();
        // std::rename() replaces the target, as the SFTP rename extension does.
        if (std::rename(QFile::encodeName(temp).constData(),
                        QFile::encodeName(target).constData()) != 0) {
            return core::fail(core::ErrorCode::Io, target);
        }
        return entry(QFileInfo(target));
    }

    core::Result<bool> create(const QString &path, const QByteArray &data) override
    {
        QFile file(absolute(path));
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            if (file.exists()) {
                return false;
            }
            return core::fail(core::ErrorCode::Io, file.fileName());
        }
        file.write(data);
        return true;
    }

    core::Result<void> rename(const QString &from, const QString &to) override
    {
        if (!QFileInfo::exists(absolute(from))) {
            return core::fail(core::ErrorCode::NotFound, absolute(from));
        }
        if (std::rename(QFile::encodeName(absolute(from)).constData(),
                        QFile::encodeName(absolute(to)).constData()) != 0) {
            return core::fail(core::ErrorCode::Io, absolute(to));
        }
        return {};
    }

    core::Result<void> remove(const QString &path) override
    {
        QFile file(absolute(path));
        if (file.exists() && !file.remove()) {
            return core::fail(core::ErrorCode::Io, file.fileName());
        }
        return {};
    }

    core::Result<void> removeFolder(const QString &path) override
    {
        if (!QDir(absolute(path)).removeRecursively()) {
            return core::fail(core::ErrorCode::Io, absolute(path));
        }
        return {};
    }

    core::Result<void> makeFolder(const QString &path) override
    {
        if (!QDir().mkpath(absolute(path))) {
            return core::fail(core::ErrorCode::Io, absolute(path));
        }
        return {};
    }

    /// Sets a file's modification time back, as an interrupted upload long ago would leave it.
    void age(const QString &path, qint64 days)
    {
        QFile file(absolute(path));
        if (file.open(QIODevice::ReadWrite)) {
            file.setFileTime(QDateTime::currentDateTimeUtc().addDays(-days),
                             QFileDevice::FileModificationTime);
        }
    }

private:
    QString absolute(const QString &path) const
    {
        return path.isEmpty() ? m_root : m_root + u'/' + path;
    }

    static core::RemoteEntry entry(const QFileInfo &info)
    {
        return {.name = info.fileName(),
                .folder = info.isDir(),
                .size = info.isDir() ? 0 : info.size(),
                .modified = info.lastModified().toMSecsSinceEpoch()};
    }

    QString m_root;
};
