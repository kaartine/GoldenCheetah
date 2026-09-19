/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_RIDEREFRESHITEMINPUTS_H
#define GC_RIDEREFRESHITEMINPUTS_H

#include <QByteArray>
#include <QColor>
#include <QDateTime>
#include <QMap>
#include <QString>
#include <QThread>

#include <optional>

class RideItem;

inline QString rideRefreshItemText(
    const QMap<QString, QString> &metadata,
    const QDateTime &dateTime,
    const QString &name,
    const QString &fallback)
{
    if (name == QStringLiteral("Start Date")) {
        return QString::number(
            QDate(1900, 1, 1).daysTo(dateTime.date()));
    }
    if (name == QStringLiteral("Start Time")) {
        return QString::number(
            QTime(0, 0, 0).secsTo(dateTime.time()));
    }
    return metadata.value(name, fallback);
}

inline unsigned long rideRefreshMetadataCrc(
    const QMap<QString, QString> &metadata)
{
    QByteArray bytes;
    for (auto it = metadata.cbegin(); it != metadata.cend(); ++it) {
        if (it.key() == QStringLiteral("Calendar Text")) continue;
        bytes.append(it.key().toUtf8());
        bytes.append(it.value().toUtf8());
    }
    return qChecksum(bytes);
}

struct RideRefreshItemStaleInputs {
    quint64 generation = 0;
    bool initiallyStale = true;
    QColor color;
    int storedUserMetricSchemaVersion = 0;
    std::optional<quint16> requiredUserMetricSchemaVersion;
    int storedDbVersion = 0;
    std::optional<unsigned long> storedWeightMilligrams;
    std::optional<double> resolvedWeight;
    std::optional<unsigned long> resolvedWeightMilligrams;
    unsigned long storedRefreshFingerprint = 0;
    std::optional<unsigned long> refreshFingerprint;
    QString sourcePath;
    unsigned long storedTimestamp = 0;
    unsigned long storedCrc = 0;
    bool samples = false;
    bool hasIntervals = false;
    unsigned long storedMetadataCrc = 0;
    unsigned long currentMetadataCrc = 0;
    bool cacheStoragePathsComplete = false;
    QString cachePath;
    double cacheWeight = 0.0;
    QByteArray cacheAnalysisFingerprint;
};

struct RideRefreshWorkItem {
    RideItem *target = nullptr;
    RideRefreshItemStaleInputs inputs;
};

struct RideRefreshItemGateDecision {
    bool stale = true;
    bool applyColor = false;
    bool writeResolvedWeight = false;
    bool continueWithSourceChecks = false;
};

struct RideRefreshItemSourceDecision {
    bool stale = false;
    std::optional<unsigned int> crcUpdate;
};

inline bool rideRefreshCaptureThreadAllowed(
    const QThread *current, const QThread *owner)
{
    return current && current == owner;
}

inline bool rideRefreshWorksetCaptureAllowed(
    bool empty, bool generationAccepted)
{
    return empty || generationAccepted;
}

inline RideRefreshItemGateDecision rideRefreshItemGateDecision(
    const RideRefreshItemStaleInputs &inputs,
    quint64 environmentGeneration,
    int requiredDbVersion)
{
    RideRefreshItemGateDecision decision;
    if (inputs.generation != environmentGeneration
        || inputs.initiallyStale) {
        return decision;
    }

    decision.applyColor = true;
    if (!inputs.requiredUserMetricSchemaVersion
        || inputs.storedUserMetricSchemaVersion
            != *inputs.requiredUserMetricSchemaVersion
        || inputs.storedDbVersion != requiredDbVersion
        || !inputs.resolvedWeight) {
        return decision;
    }

    decision.writeResolvedWeight = true;
    if (!inputs.storedWeightMilligrams
        || !inputs.resolvedWeightMilligrams
        || *inputs.storedWeightMilligrams
            != *inputs.resolvedWeightMilligrams
        || !inputs.refreshFingerprint
        || inputs.storedRefreshFingerprint
            != *inputs.refreshFingerprint) {
        return decision;
    }

    decision.stale = false;
    decision.continueWithSourceChecks = true;
    return decision;
}

inline RideRefreshItemSourceDecision rideRefreshItemSourceDecision(
    const RideRefreshItemStaleInputs &inputs,
    qint64 sourceModifiedSeconds,
    const std::optional<unsigned int> &computedCrc)
{
    RideRefreshItemSourceDecision decision;
    if (inputs.storedTimestamp
        < static_cast<unsigned long>(qMax<qint64>(0, sourceModifiedSeconds))) {
        if (!computedCrc) {
            decision.stale = true;
        } else if (inputs.storedCrc == 0
                   || inputs.storedCrc != *computedCrc) {
            decision.stale = true;
            decision.crcUpdate = *computedCrc;
        }
    }
    if (inputs.samples && !inputs.hasIntervals) decision.stale = true;
    return decision;
}

#endif // GC_RIDEREFRESHITEMINPUTS_H
