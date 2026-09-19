/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideRefreshMeasures.h"

#include "Measures.h"

#include <utility>

RideRefreshMeasures::Group captureRideRefreshMeasuresGroup(
    MeasuresGroup *source)
{
    RideRefreshMeasures::Group result;
    if (!source) return result;

    result.symbol = source->getSymbol();
    result.name = source->getName();
    const QStringList symbols = source->getFieldSymbols();
    result.fields.reserve(symbols.size());
    for (int index = 0; index < symbols.size(); ++index) {
        const MeasuresField field = source->getField(index);
        result.fields.append({
            field.symbol,
            field.name,
            field.metricUnits,
            field.imperialUnits,
            field.unitsFactor,
            field.headers
        });
    }

    const QList<Measure> &observations = source->measures();
    result.observations.reserve(observations.size());
    for (const Measure &measure : observations) {
        RideRefreshMeasures::Observation observation;
        observation.when = measure.when;
        observation.comment = measure.comment;
        observation.source = int(measure.source);
        observation.originalSource = measure.originalSource;
        static_assert(
            RideRefreshMeasures::MaximumFields == MAX_MEASURES,
            "refresh snapshot must retain every legacy measure slot");
        for (int index = 0; index < MAX_MEASURES; ++index)
            observation.values[index] = measure.values[index];
        observation.legacyFingerprint = measure.getFingerprint();
        result.observations.append(std::move(observation));
    }
    return result;
}
