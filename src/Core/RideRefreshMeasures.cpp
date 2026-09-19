/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideRefreshMeasures.h"

#include <utility>

std::shared_ptr<const RideRefreshMeasures> RideRefreshMeasures::create(
    QVector<Group> groups,
    quint16 missingObservationFingerprint)
{
    QHash<QString, int> groupIndexes;
    for (int index = 0; index < groups.size(); ++index) {
        // Legacy symbol lookup uses indexOf(), so the first duplicate wins.
        if (!groupIndexes.contains(groups.at(index).symbol))
            groupIndexes.insert(groups.at(index).symbol, index);
    }

    return std::shared_ptr<const RideRefreshMeasures>(
        new RideRefreshMeasures(
            std::move(groups), std::move(groupIndexes),
            missingObservationFingerprint));
}

RideRefreshMeasures::RideRefreshMeasures(
    QVector<Group> groups,
    QHash<QString, int> groupIndexes,
    quint16 missingObservationFingerprint)
    : groups_(std::move(groups))
    , groupIndexes_(std::move(groupIndexes))
    , missingObservationFingerprint_(missingObservationFingerprint)
{
}

QStringList RideRefreshMeasures::groupSymbols() const
{
    QStringList result;
    result.reserve(groups_.size());
    for (const Group &group : groups_) result.append(group.symbol);
    return result;
}

QStringList RideRefreshMeasures::groupNames() const
{
    QStringList result;
    result.reserve(groups_.size());
    for (const Group &group : groups_) result.append(group.name);
    return result;
}

const RideRefreshMeasures::Group *RideRefreshMeasures::group(
    const QString &symbol) const
{
    const auto found = groupIndexes_.constFind(symbol);
    return found == groupIndexes_.cend() ? nullptr : &groups_.at(*found);
}

const RideRefreshMeasures::Group *RideRefreshMeasures::group(int index) const
{
    return index >= 0 && index < groups_.size() ? &groups_.at(index) : nullptr;
}

const RideRefreshMeasures::Field *RideRefreshMeasures::field(
    const QString &groupSymbol, int index) const
{
    const Group *found = group(groupSymbol);
    return found && index >= 0 && index < found->fields.size()
        ? &found->fields.at(index) : nullptr;
}

const RideRefreshMeasures::Field *RideRefreshMeasures::field(
    const QString &groupSymbol, const QString &fieldSymbol) const
{
    const Group *found = group(groupSymbol);
    if (!found) return nullptr;
    for (const Field &candidate : found->fields) {
        if (candidate.symbol == fieldSymbol) return &candidate;
    }
    return nullptr;
}

const RideRefreshMeasures::Observation *
RideRefreshMeasures::observationForDate(
    const Group &group, const QDate &date)
{
    const Observation *result = nullptr;
    for (const Observation &observation : group.observations) {
        const QDate observationDate = observation.when.date();
        if (group.symbol == QStringLiteral("Body")
            && observationDate < date) {
            result = &observation;
        }
        if (observationDate == date) result = &observation;
        if (observationDate > date) break;
    }
    return result;
}

const RideRefreshMeasures::Observation *
RideRefreshMeasures::observationForDate(
    const QString &groupSymbol, const QDate &date) const
{
    const Group *found = group(groupSymbol);
    return found ? observationForDate(*found, date) : nullptr;
}

const RideRefreshMeasures::Observation *
RideRefreshMeasures::observationForDate(int groupIndex, const QDate &date) const
{
    const Group *found = group(groupIndex);
    return found ? observationForDate(*found, date) : nullptr;
}

std::optional<QDate> RideRefreshMeasures::startDate(
    const QString &groupSymbol) const
{
    const Group *found = group(groupSymbol);
    if (!found) return std::nullopt;
    return found->observations.isEmpty()
        ? QDate() : found->observations.first().when.date();
}

std::optional<QDate> RideRefreshMeasures::startDate(int groupIndex) const
{
    const Group *found = group(groupIndex);
    if (!found) return std::nullopt;
    return found->observations.isEmpty()
        ? QDate() : found->observations.first().when.date();
}

std::optional<QDate> RideRefreshMeasures::endDate(
    const QString &groupSymbol) const
{
    const Group *found = group(groupSymbol);
    if (!found) return std::nullopt;
    return found->observations.isEmpty()
        ? QDate() : found->observations.last().when.date();
}

std::optional<QDate> RideRefreshMeasures::endDate(int groupIndex) const
{
    const Group *found = group(groupIndex);
    if (!found) return std::nullopt;
    return found->observations.isEmpty()
        ? QDate() : found->observations.last().when.date();
}

std::optional<QStringList> RideRefreshMeasures::fieldSymbols(
    const QString &groupSymbol) const
{
    const Group *found = group(groupSymbol);
    if (!found) return std::nullopt;

    QStringList result;
    result.reserve(found->fields.size());
    for (const Field &field : found->fields) result.append(field.symbol);
    return result;
}

std::optional<QStringList> RideRefreshMeasures::fieldSymbols(int groupIndex) const
{
    const Group *found = group(groupIndex);
    if (!found) return std::nullopt;
    QStringList result;
    result.reserve(found->fields.size());
    for (const Field &field : found->fields) result.append(field.symbol);
    return result;
}

std::optional<QString> RideRefreshMeasures::fieldUnits(
    const QString &groupSymbol,
    int fieldIndex,
    bool useMetricUnits) const
{
    const Field *found = field(groupSymbol, fieldIndex);
    if (!found) return std::nullopt;
    return useMetricUnits ? found->metricUnits : found->imperialUnits;
}

std::optional<QString> RideRefreshMeasures::fieldUnits(
    const QString &groupSymbol,
    const QString &fieldSymbol,
    bool useMetricUnits) const
{
    const Field *found = field(groupSymbol, fieldSymbol);
    if (!found) return std::nullopt;
    return useMetricUnits ? found->metricUnits : found->imperialUnits;
}

std::optional<double> RideRefreshMeasures::fieldValue(
    const QString &groupSymbol,
    const QDate &date,
    int field,
    bool useMetricUnits) const
{
    const Group *found = group(groupSymbol);
    if (!found || field < 0 || field >= found->fields.size()
        || field >= MaximumFields) {
        return std::nullopt;
    }

    const Observation *observation = observationForDate(*found, date);
    const double value = observation ? observation->values[field] : 0.0;
    return value * (useMetricUnits ? 1.0 : found->fields[field].unitsFactor);
}

std::optional<double> RideRefreshMeasures::fieldValue(
    int groupIndex,
    const QDate &date,
    int fieldIndex,
    bool useMetricUnits) const
{
    const Group *found = group(groupIndex);
    if (!found) return std::nullopt;
    return fieldValue(found->symbol, date, fieldIndex, useMetricUnits);
}

std::optional<double> RideRefreshMeasures::fieldValue(
    const QString &groupSymbol,
    const QDate &date,
    const QString &fieldSymbol,
    bool useMetricUnits) const
{
    const Group *found = group(groupSymbol);
    if (!found) return std::nullopt;

    int fieldIndex = -1;
    for (int index = 0; index < found->fields.size(); ++index) {
        if (found->fields.at(index).symbol == fieldSymbol) {
            fieldIndex = index;
            break;
        }
    }
    if (fieldIndex < 0) return std::nullopt;
    return fieldValue(groupSymbol, date, fieldIndex, useMetricUnits);
}

std::optional<quint16> RideRefreshMeasures::fingerprint(
    const QString &groupSymbol, const QDate &date) const
{
    const Group *found = group(groupSymbol);
    if (!found) return std::nullopt;

    const Observation *observation = observationForDate(*found, date);
    return observation ? observation->fingerprint()
                       : missingObservationFingerprint_;
}

std::optional<quint16> RideRefreshMeasures::fingerprint(
    int groupIndex, const QDate &date) const
{
    const Group *found = group(groupIndex);
    if (!found) return std::nullopt;
    const Observation *observation = observationForDate(*found, date);
    return observation ? observation->fingerprint()
                       : missingObservationFingerprint_;
}
