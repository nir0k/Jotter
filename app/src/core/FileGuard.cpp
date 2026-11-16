#include "core/FileGuard.h"

#include <QDir>
#include <QFileInfo>

#include <map>

namespace core {
namespace {

/// Reuses a mutex only while a transaction is live; the registry holds no library state.
std::shared_ptr<std::recursive_mutex> mutexFor(const QString &path)
{
    static std::mutex registryLock;
    static std::map<QString, std::weak_ptr<std::recursive_mutex>> registry;
    const std::scoped_lock lock(registryLock);
    std::erase_if(registry, [](const auto &entry) { return entry.second.expired(); });
    auto &entry = registry[QDir::cleanPath(QFileInfo(path).absoluteFilePath())];
    auto mutex = entry.lock();
    if (!mutex) {
        mutex = std::make_shared<std::recursive_mutex>();
        entry = mutex;
    }
    return mutex;
}

} // namespace

FileGuard::FileGuard(const QString &path)
    : m_mutex(mutexFor(path))
    , m_lock(*m_mutex)
{}

} // namespace core
