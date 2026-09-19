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

struct RideRefreshTargetToken {
    quint64 cacheEpoch = 0;
    quint64 targetId = 0;
    quint64 revision = 0;

    bool isValid() const
    {
        return cacheEpoch != 0 && targetId != 0 && revision != 0;
    }

    friend bool operator==(
        const RideRefreshTargetToken &left,
        const RideRefreshTargetToken &right)
    {
        return left.cacheEpoch == right.cacheEpoch
            && left.targetId == right.targetId
            && left.revision == right.revision;
    }
};

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
    bool backgroundRefreshAllowed = false;
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
    RideRefreshTargetToken targetToken;
    qsizetype workIndex = -1;
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

struct RideRefreshStaleProposal {
    bool applyColor = false;
    QColor color;
    std::optional<double> resolvedWeight;
    std::optional<unsigned int> crcUpdate;
    bool stale = true;
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

inline bool rideRefreshBackgroundBuildAllowed(
    bool open, bool dirty, bool editing)
{
    return !open && !dirty && !editing;
}

inline bool rideRefreshMutableItemThreadAllowed(
    const QThread *current,
    const QThread *itemOwner,
    const QThread *cacheOwner)
{
    return current && itemOwner && cacheOwner
        && current == itemOwner;
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

template<typename SourceModified, typename SourceCrc, typename CacheStale>
RideRefreshStaleProposal evaluateRideRefreshStaleness(
    const RideRefreshItemStaleInputs &inputs,
    quint64 environmentGeneration,
    int requiredDbVersion,
    SourceModified &&sourceModified,
    SourceCrc &&sourceCrc,
    CacheStale &&cacheStale)
{
    RideRefreshStaleProposal proposal;
    const RideRefreshItemGateDecision gate =
        rideRefreshItemGateDecision(
            inputs, environmentGeneration, requiredDbVersion);
    proposal.applyColor = gate.applyColor;
    if (!proposal.applyColor) return proposal;

    proposal.color = inputs.color;
    if (gate.writeResolvedWeight)
        proposal.resolvedWeight = inputs.resolvedWeight;
    if (!gate.continueWithSourceChecks) return proposal;

    const qint64 modifiedSeconds = sourceModified();
    std::optional<unsigned int> computedCrc;
    if (inputs.storedTimestamp < static_cast<unsigned long>(
            qMax<qint64>(0, modifiedSeconds))) {
        computedCrc = sourceCrc();
    }
    const RideRefreshItemSourceDecision sourceDecision =
        rideRefreshItemSourceDecision(
            inputs, modifiedSeconds, computedCrc);
    proposal.crcUpdate = sourceDecision.crcUpdate;
    proposal.stale = sourceDecision.stale;
    if (!proposal.stale) proposal.stale = cacheStale();
    if (inputs.storedMetadataCrc != inputs.currentMetadataCrc)
        proposal.stale = true;
    return proposal;
}

template<typename Evaluate>
bool evaluateAndApplyRideRefreshStaleness(
    Evaluate &&evaluate,
    QColor &color,
    double &weight,
    unsigned long &crc,
    bool &stale)
{
    const RideRefreshStaleProposal proposal = evaluate();
    if (proposal.applyColor) color = proposal.color;
    if (proposal.resolvedWeight) weight = *proposal.resolvedWeight;
    if (proposal.crcUpdate) crc = *proposal.crcUpdate;
    stale = proposal.stale;
    return stale;
}

#endif // GC_RIDEREFRESHITEMINPUTS_H
