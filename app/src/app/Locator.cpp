#include "app/Locator.h"

#include "core/Files.h"

#include <QGeoPositionInfoSource>
#include <QTimer>

#include <cmath>

namespace {

/// A position not available within 10 seconds is skipped.
constexpr int TimeoutMs = 10000;

/// Rounds a coordinate to the five decimals a note stores, about a metre.
double rounded(double degrees)
{
    return std::round(degrees * 100000.0) / 100000.0;
}

} // namespace

bool Locator::isAvailable()
{
    return !QGeoPositionInfoSource::availableSources().isEmpty();
}

void Locator::locate(const QString &noteId)
{
    QGeoPositionInfoSource *source = QGeoPositionInfoSource::createDefaultSource(this);
    if (source == nullptr) {
        // Reported after the caller returns, like an answer from the service.
        QTimer::singleShot(0, this, [this, noteId] { Q_EMIT failed(noteId); });
        return;
    }
    connect(source, &QGeoPositionInfoSource::positionUpdated, this,
            [this, source, noteId](const QGeoPositionInfo &info) {
                source->deleteLater();
                if (!info.isValid()) {
                    Q_EMIT failed(noteId);
                    return;
                }
                core::GeoLocation location;
                location.latitude = rounded(info.coordinate().latitude());
                location.longitude = rounded(info.coordinate().longitude());
                if (info.hasAttribute(QGeoPositionInfo::HorizontalAccuracy)) {
                    location.accuracy = info.attribute(QGeoPositionInfo::HorizontalAccuracy);
                }
                location.captured = core::now();
                Q_EMIT located(noteId, location);
            });
    connect(source, &QGeoPositionInfoSource::errorOccurred, this,
            [this, source, noteId](QGeoPositionInfoSource::Error /*error*/) {
                source->deleteLater();
                Q_EMIT failed(noteId);
            });
    source->requestUpdate(TimeoutMs);
}
