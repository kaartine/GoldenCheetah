/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_RIDECACHEAGGREGATE_H
#define GC_RIDECACHEAGGREGATE_H

#include <QVector>

#include <cmath>

namespace RideCacheAggregate {

enum class MetricType {
    Total,
    Average,
    Peak,
    Low,
    RunningTotal,
    MeanSquareRoot
};

struct MetricDefinition
{
    MetricType type = MetricType::Total;
    bool aggregateZero = false;
    bool excludesValue = false;
    double excludedValue = 0.0;
    bool enabled = true;
    bool divideByCount = true;
};

struct Accumulator
{
    double value = 0.0;
    double count = 0.0;
};

inline void add(
    Accumulator &accumulator,
    const MetricDefinition &metric,
    double value,
    double count)
{
    if (!std::isfinite(value)) value = 0.0;

    bool aggregateZero = metric.aggregateZero;
    if (metric.excludesValue && value == metric.excludedValue) {
        value = 0.0;
        aggregateZero = false;
    }

    switch (metric.type) {
    case MetricType::RunningTotal:
    case MetricType::Total:
        accumulator.value += value;
        break;
    case MetricType::Average:
        if (value != 0.0 || aggregateZero) {
            accumulator.value += value * count;
            accumulator.count += count;
        }
        break;
    case MetricType::Low:
        if (value < accumulator.value) accumulator.value = value;
        break;
    case MetricType::Peak:
        if (value > accumulator.value) accumulator.value = value;
        break;
    case MetricType::MeanSquareRoot:
        accumulator.value = std::sqrt(
            (std::pow(accumulator.value, 2) * accumulator.count
             + std::pow(value, 2) * count)
            / (accumulator.count + count));
        accumulator.count += count;
        break;
    }
}

inline double finalValue(
    const Accumulator &accumulator,
    const MetricDefinition &metric)
{
    if (metric.type == MetricType::Average
        && metric.divideByCount
        && accumulator.count != 0.0) {
        return accumulator.value / accumulator.count;
    }
    return accumulator.value;
}

struct BatchResult
{
    QVector<QVector<Accumulator>> accumulators;
};

template<typename ItemRange,
         typename SpecificationRange,
         typename Pass,
         typename Value,
         typename Count>
BatchResult aggregate(
    const ItemRange &items,
    const SpecificationRange &specifications,
    const QVector<MetricDefinition> &metrics,
    Pass pass,
    Value value,
    Count count)
{
    BatchResult result;
    result.accumulators.resize(specifications.size());
    for (QVector<Accumulator> &values : result.accumulators) {
        values.resize(metrics.size());
    }
    if (specifications.isEmpty() || metrics.isEmpty()) return result;

    bool hasEnabledMetric = false;
    for (const MetricDefinition &metric : metrics) {
        if (metric.enabled) {
            hasEnabledMetric = true;
            break;
        }
    }
    if (!hasEnabledMetric) return result;

    QVector<qsizetype> matchingSpecifications;
    matchingSpecifications.reserve(specifications.size());
    for (const auto &item : items) {
        matchingSpecifications.clear();
        for (qsizetype specification = 0;
             specification < specifications.size();
             ++specification) {
            if (pass(specifications.at(specification), item)) {
                matchingSpecifications.append(specification);
            }
        }
        if (matchingSpecifications.isEmpty()) continue;

        for (qsizetype metric = 0; metric < metrics.size(); ++metric) {
            const MetricDefinition &definition = metrics.at(metric);
            if (!definition.enabled) continue;

            const double itemValue = value(metric, item);
            const double itemCount = count(metric, item);
            for (qsizetype specification : matchingSpecifications) {
                add(
                    result.accumulators[specification][metric],
                    definition,
                    itemValue,
                    itemCount);
            }
        }
    }
    return result;
}

template<typename ItemRange,
         typename SpecificationRange,
         typename Pass,
         typename Relevant>
QVector<bool> metricRelevance(
    const ItemRange &items,
    const SpecificationRange &specifications,
    qsizetype metricCount,
    Pass pass,
    Relevant relevant)
{
    QVector<bool> result(metricCount, false);
    if (specifications.isEmpty() || metricCount <= 0) return result;

    qsizetype unresolved = metricCount;
    for (const auto &item : items) {
        bool matches = false;
        for (const auto &specification : specifications) {
            if (pass(specification, item)) {
                matches = true;
                break;
            }
        }
        if (!matches) continue;

        for (qsizetype metric = 0; metric < metricCount; ++metric) {
            if (!result.at(metric) && relevant(metric, item)) {
                result[metric] = true;
                --unresolved;
            }
        }
        if (unresolved == 0) break;
    }
    return result;
}

} // namespace RideCacheAggregate

#endif // GC_RIDECACHEAGGREGATE_H
