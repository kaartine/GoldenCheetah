/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_RIDEREFRESHROUTES_H
#define GC_RIDEREFRESHROUTES_H

#include <QString>
#include <QUuid>
#include <QVector>

#include <memory>

class RideRefreshRoutes final
{
public:
    struct Point {
        double longitude = 0.0;
        double latitude = 0.0;
    };

    struct Segment {
        QUuid id;
        QString name;
        QVector<Point> points;
        double minimumLatitude = 180.0;
        double maximumLatitude = -180.0;
        double minimumLongitude = 180.0;
        double maximumLongitude = -180.0;
    };

    struct RidePoint {
        double seconds = 0.0;
        double kilometres = 0.0;
        double longitude = 0.0;
        double latitude = 0.0;
    };

    struct Ride {
        QVector<RidePoint> points;
        double minimumLatitude = 0.0;
        double maximumLatitude = 0.0;
        double minimumLongitude = 0.0;
        double maximumLongitude = 0.0;
    };

    struct Match {
        QUuid routeId;
        QString name;
        double startSeconds = 0.0;
        double stopSeconds = 0.0;
        double startKilometres = 0.0;
        double stopKilometres = 0.0;
        int occurrence = 0;
    };

    struct SearchParameters {
        double minimumPrecisionKilometres = 0.100;
        double maximumPrecisionKilometres = 0.001;
        double boundsToleranceDegrees = 0.001;
        int farPointSkip = 50;
        int lookAhead = 10;
        int maximumDivergence = 2;
        int restartSkip = 30;
    };

    static std::shared_ptr<const RideRefreshRoutes> create(
        QVector<Segment> segments,
        quint16 fingerprint);
    static std::shared_ptr<const RideRefreshRoutes> create(
        QVector<Segment> segments,
        quint16 fingerprint,
        SearchParameters parameters);

    const QVector<Segment> &segments() const { return segments_; }
    quint16 fingerprint() const { return fingerprint_; }
    const SearchParameters &parameters() const { return parameters_; }
    QVector<Match> search(const Ride &ride) const;
    static double distanceAt(const Ride &ride, double seconds);

private:
    RideRefreshRoutes(
        QVector<Segment> segments,
        quint16 fingerprint,
        SearchParameters parameters);

    static QVector<Match> searchSegment(
        const Segment &segment,
        const Ride &ride,
        const SearchParameters &parameters);
    static double distanceKilometres(
        double latitude1,
        double longitude1,
        double latitude2,
        double longitude2);
    const QVector<Segment> segments_;
    const quint16 fingerprint_;
    const SearchParameters parameters_;
};

#endif // GC_RIDEREFRESHROUTES_H
