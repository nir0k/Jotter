#pragma once

#include <QString>

#include <memory>
#include <mutex>

namespace core {

/// Serialises short local read/check/write transactions for one file. Remote operations must
/// happen outside this guard; recursive acquisition allows existing atomic-write helpers.
class FileGuard
{
public:
    /// Locks the process-local mutation boundary of an absolute file path.
    explicit FileGuard(const QString &path);

private:
    std::shared_ptr<std::recursive_mutex> m_mutex;
    std::unique_lock<std::recursive_mutex> m_lock;
};

} // namespace core
