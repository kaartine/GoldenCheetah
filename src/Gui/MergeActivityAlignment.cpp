/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "MergeActivityAlignment.h"

#include <QtConcurrent>

#include <gsl/gsl_errno.h>
#include <gsl/gsl_fft_complex.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <limits>
#include <utility>
#include <vector>

namespace MergeActivityAlignment {
namespace {

constexpr int DirectSearchLimit = 512;
constexpr int ExactCandidateLimit = 64;
constexpr double NearTieTolerance = 1.0e-7;

bool isCancelled(const std::atomic_bool *cancellation)
{
    return cancellation &&
           cancellation->load(std::memory_order_relaxed);
}

struct ScoreResult
{
    double score = std::numeric_limits<double>::quiet_NaN();
    bool cancelled = false;
};

ScoreResult directScore(
    const QVector<double> &base,
    const QVector<double> &fit,
    int offset,
    const std::atomic_bool *cancellation)
{
    ScoreResult result;
    if (isCancelled(cancellation)) {
        result.cancelled = true;
        return result;
    }

    double mean = 0.0;
    int meanCount = 0;
    for (int index = 0; index + offset < base.size(); ++index) {
        if ((index & 4095) == 0 && isCancelled(cancellation)) {
            result.cancelled = true;
            return result;
        }
        const int baseIndex = index + offset;
        if (baseIndex <= 0) continue;

        const double baseValue = base.at(baseIndex);
        if (!std::isfinite(baseValue)) return result;
        mean += baseValue;
        ++meanCount;
    }
    if (meanCount == 0) return result;
    mean /= double(meanCount);
    if (!std::isfinite(mean)) return result;

    double residual = 0.0;
    double total = 0.0;
    double firstBaseValue = 0.0;
    double firstFitValue = 0.0;
    bool hasOverlap = false;
    bool baseVaries = false;
    bool fitVaries = false;
    for (int index = 0;
         index + offset < base.size() && index < fit.size();
         ++index) {
        if ((index & 4095) == 0 && isCancelled(cancellation)) {
            result.cancelled = true;
            return result;
        }
        const int baseIndex = index + offset;
        if (baseIndex <= 0) continue;

        const double baseValue = base.at(baseIndex);
        const double fitValue = fit.at(index);
        if (!std::isfinite(baseValue) || !std::isfinite(fitValue)) {
            return result;
        }
        if (!hasOverlap) {
            firstBaseValue = baseValue;
            firstFitValue = fitValue;
            hasOverlap = true;
        } else {
            baseVaries = baseVaries || baseValue != firstBaseValue;
            fitVaries = fitVaries || fitValue != firstFitValue;
        }

        const double delta = baseValue - fitValue;
        const double centered = baseValue - mean;
        residual += delta * delta;
        total += centered * centered;
    }

    if (!hasOverlap || !baseVaries || !fitVaries ||
        !(total > 0.0) || !std::isfinite(residual) ||
        !std::isfinite(total)) {
        return result;
    }
    result.score = 1.0 - residual / total;
    if (!std::isfinite(result.score)) {
        result.score = std::numeric_limits<double>::quiet_NaN();
    }
    return result;
}

Result directBestOffset(
    const QVector<double> &base,
    const QVector<double> &fit,
    const std::atomic_bool *cancellation)
{
    Result result;
    const int searchRadius = base.size() / 3;
    for (int offset = -searchRadius; offset < searchRadius; ++offset) {
        const ScoreResult candidate =
            directScore(base, fit, offset, cancellation);
        if (candidate.cancelled) {
            result = Result();
            result.cancelled = true;
            return result;
        }
        if (candidate.score > result.rSquared) {
            result.valid = true;
            result.offset = offset;
            result.rSquared = candidate.score;
        }
    }
    return result;
}

bool allFiniteConstant(
    const QVector<double> &values,
    int firstIndex)
{
    if (firstIndex >= values.size()) return false;

    const double first = values.at(firstIndex);
    if (!std::isfinite(first)) return false;
    for (int index = firstIndex + 1; index < values.size(); ++index) {
        if (!std::isfinite(values.at(index)) ||
            values.at(index) != first) {
            return false;
        }
    }
    return true;
}

std::size_t nextPowerOfTwo(std::size_t value)
{
    std::size_t result = 1;
    while (result < value) {
        if (result > std::numeric_limits<std::size_t>::max() / 2) {
            return 0;
        }
        result <<= 1;
    }
    return result;
}

bool invalidInRange(
    const QVector<int> &prefix,
    int begin,
    int end)
{
    return begin < end && prefix.at(end) != prefix.at(begin);
}

bool variesInRange(
    const QVector<int> &changes,
    int begin,
    int end)
{
    return end - begin > 1 &&
           changes.at(end) != changes.at(begin + 1);
}

struct Candidate
{
    int offset = 0;
    double approximateScore = 0.0;
};

void appendUnique(QVector<int> &offsets, int offset)
{
    if (!offsets.contains(offset)) offsets.append(offset);
}

Result fftBestOffset(
    const QVector<double> &base,
    const QVector<double> &fit,
    const std::atomic_bool *cancellation)
{
    Result result;
    if (isCancelled(cancellation)) {
        result.cancelled = true;
        return result;
    }

    const int baseCount = base.size();
    const int fitCount = fit.size();
    const int searchRadius = baseCount / 3;
    if (baseCount == 0 || fitCount == 0 || searchRadius == 0) {
        return result;
    }

    double commonScale = 0.0;
    for (double value : base) {
        if (std::isfinite(value)) {
            commonScale = std::max(commonScale, std::abs(value));
        }
    }
    for (double value : fit) {
        if (std::isfinite(value)) {
            commonScale = std::max(commonScale, std::abs(value));
        }
    }
    if (!(commonScale > 0.0) || !std::isfinite(commonScale)) {
        return result;
    }

    QVector<double> scaledBase(baseCount, 0.0);
    QVector<double> scaledFit(fitCount, 0.0);
    QVector<long double> baseSum(baseCount + 1, 0.0L);
    QVector<long double> baseSquares(baseCount + 1, 0.0L);
    QVector<long double> fitSquares(fitCount + 1, 0.0L);
    QVector<int> baseInvalid(baseCount + 1, 0);
    QVector<int> fitInvalid(fitCount + 1, 0);
    QVector<int> baseChanges(baseCount + 1, 0);
    QVector<int> fitChanges(fitCount + 1, 0);

    for (int index = 0; index < baseCount; ++index) {
        if ((index & 4095) == 0 && isCancelled(cancellation)) {
            result.cancelled = true;
            return result;
        }
        const bool finite = std::isfinite(base.at(index));
        double value = finite ? base.at(index) / commonScale : 0.0;
        if (index == 0) value = 0.0;
        scaledBase[index] = value;
        baseSum[index + 1] = baseSum.at(index) + value;
        baseSquares[index + 1] =
            baseSquares.at(index) + (static_cast<long double>(value) * value);
        baseInvalid[index + 1] =
            baseInvalid.at(index) + ((!finite && index > 0) ? 1 : 0);
        baseChanges[index + 1] =
            baseChanges.at(index) +
            ((index > 0 && base.at(index) != base.at(index - 1)) ? 1 : 0);
    }
    for (int index = 0; index < fitCount; ++index) {
        if ((index & 4095) == 0 && isCancelled(cancellation)) {
            result.cancelled = true;
            return result;
        }
        const bool finite = std::isfinite(fit.at(index));
        const double value = finite ? fit.at(index) / commonScale : 0.0;
        scaledFit[index] = value;
        fitSquares[index + 1] =
            fitSquares.at(index) + (static_cast<long double>(value) * value);
        fitInvalid[index + 1] =
            fitInvalid.at(index) + (finite ? 0 : 1);
        fitChanges[index + 1] =
            fitChanges.at(index) +
            ((index > 0 && fit.at(index) != fit.at(index - 1)) ? 1 : 0);
    }

    if (allFiniteConstant(base, 1) || allFiniteConstant(fit, 0)) {
        return result;
    }

    std::vector<double> convolution;
    std::size_t fftSize = 0;
    int fftStages = 0;
    {
        const std::size_t convolutionCount =
            std::size_t(baseCount) + std::size_t(fitCount) - 1;
        fftSize = nextPowerOfTwo(convolutionCount);
        if (fftSize == 0 ||
            fftSize > std::numeric_limits<std::size_t>::max() / 2) {
            return result;
        }

        std::size_t stageSize = fftSize;
        while (stageSize > 1) {
            ++fftStages;
            stageSize >>= 1;
        }

        std::vector<double> baseTransform(2 * fftSize, 0.0);
        std::vector<double> fitTransform(2 * fftSize, 0.0);
        for (int index = 0; index < baseCount; ++index) {
            baseTransform[2 * std::size_t(index)] = scaledBase.at(index);
        }
        for (int index = 0; index < fitCount; ++index) {
            fitTransform[2 * std::size_t(index)] =
                scaledFit.at(fitCount - 1 - index);
        }

        if (isCancelled(cancellation)) {
            result.cancelled = true;
            return result;
        }
        if (gsl_fft_complex_radix2_forward(
                baseTransform.data(), 1, fftSize) != GSL_SUCCESS) {
            return result;
        }
        if (isCancelled(cancellation)) {
            result.cancelled = true;
            return result;
        }
        if (gsl_fft_complex_radix2_forward(
                fitTransform.data(), 1, fftSize) != GSL_SUCCESS) {
            return result;
        }
        if (isCancelled(cancellation)) {
            result.cancelled = true;
            return result;
        }

        for (std::size_t index = 0; index < fftSize; ++index) {
            const double ar = baseTransform[2 * index];
            const double ai = baseTransform[2 * index + 1];
            const double br = fitTransform[2 * index];
            const double bi = fitTransform[2 * index + 1];
            baseTransform[2 * index] = ar * br - ai * bi;
            baseTransform[2 * index + 1] = ar * bi + ai * br;
        }
        if (isCancelled(cancellation)) {
            result.cancelled = true;
            return result;
        }
        if (gsl_fft_complex_radix2_inverse(
                baseTransform.data(), 1, fftSize) != GSL_SUCCESS) {
            return result;
        }
        if (isCancelled(cancellation)) {
            result.cancelled = true;
            return result;
        }

        convolution.resize(convolutionCount);
        for (std::size_t index = 0; index < convolutionCount; ++index) {
            convolution[index] = baseTransform[2 * index];
        }
    }

    QVector<Candidate> candidates;
    candidates.reserve(2 * searchRadius);
    double bestApproximate = 0.0;

    for (int offset = -searchRadius; offset < searchRadius; ++offset) {
        if ((offset & 255) == 0 && isCancelled(cancellation)) {
            result.cancelled = true;
            return result;
        }

        const int lower = std::max(0, 1 - offset);
        const int upper = std::min(fitCount, baseCount - offset);
        if (lower >= upper) continue;

        const int meanStart = std::max(1, offset);
        const int baseBegin = lower + offset;
        const int baseEnd = upper + offset;
        if (invalidInRange(baseInvalid, meanStart, baseCount) ||
            invalidInRange(baseInvalid, baseBegin, baseEnd) ||
            invalidInRange(fitInvalid, lower, upper)) {
            continue;
        }
        if (!variesInRange(baseChanges, baseBegin, baseEnd) ||
            !variesInRange(fitChanges, lower, upper)) {
            continue;
        }

        const int meanCount = baseCount - meanStart;
        if (meanCount <= 0) continue;
        const long double mean =
            (baseSum.at(baseCount) - baseSum.at(meanStart)) /
            static_cast<long double>(meanCount);
        if (!std::isfinite(mean)) continue;
        const long double sumBase =
            baseSum.at(baseEnd) - baseSum.at(baseBegin);
        const long double sumBaseSquares =
            baseSquares.at(baseEnd) - baseSquares.at(baseBegin);
        const long double sumFitSquares =
            fitSquares.at(upper) - fitSquares.at(lower);
        const int overlap = upper - lower;

        long double cross = 0.0L;
        const long long convolutionIndex =
            static_cast<long long>(fitCount - 1) + offset;
        if (convolutionIndex >= 0 &&
            std::size_t(convolutionIndex) < convolution.size()) {
            cross = convolution[std::size_t(convolutionIndex)];
        }

        long double residual =
            sumBaseSquares + sumFitSquares - 2.0L * cross;
        long double total =
            sumBaseSquares - 2.0L * mean * sumBase +
            static_cast<long double>(overlap) * mean * mean;

        {
            const long double magnitude =
                std::abs(sumBaseSquares) + std::abs(sumFitSquares) +
                2.0L * std::abs(cross) + 1.0L;
            const long double roundoff =
                256.0L * std::numeric_limits<double>::epsilon() *
                static_cast<long double>(std::max(1, fftStages)) *
                magnitude;
            if (residual < 0.0L && -residual <= roundoff) {
                residual = 0.0L;
            }
            if (total < 0.0L && -total <= roundoff) {
                total = 0.0L;
            }
        }

        if (!(total > 0.0L) || !std::isfinite(residual) ||
            !std::isfinite(total)) {
            continue;
        }

        const double score =
            1.0 - double(residual / total);
        if (!std::isfinite(score)) continue;

        Candidate candidate;
        candidate.offset = offset;
        candidate.approximateScore = score;
        candidates.append(candidate);
        if (score > bestApproximate) {
            bestApproximate = score;
        }
    }

    if (!(bestApproximate > 0.0)) return result;

    QVector<int> exactOffsets;
    for (const Candidate &candidate : candidates) {
        if (candidate.approximateScore >=
            bestApproximate - NearTieTolerance) {
            appendUnique(exactOffsets, candidate.offset);
            if (exactOffsets.size() >= ExactCandidateLimit / 2) break;
        }
    }

    QVector<Candidate> ranked = candidates;
    std::stable_sort(
        ranked.begin(), ranked.end(),
        [](const Candidate &left, const Candidate &right) {
            return left.approximateScore > right.approximateScore;
        });
    for (const Candidate &candidate : ranked) {
        appendUnique(exactOffsets, candidate.offset);
        if (exactOffsets.size() >= ExactCandidateLimit) break;
    }
    std::sort(exactOffsets.begin(), exactOffsets.end());

    for (int offset : std::as_const(exactOffsets)) {
        const ScoreResult exact =
            directScore(base, fit, offset, cancellation);
        if (exact.cancelled) {
            result = Result();
            result.cancelled = true;
            return result;
        }
        if (exact.score > result.rSquared) {
            result.valid = true;
            result.offset = offset;
            result.rSquared = exact.score;
        }
    }
    return result;
}

} // namespace

Result findBestOffset(
    const QVector<double> &base,
    const QVector<double> &fit,
    const std::atomic_bool *cancellation)
{
    if (base.size() <= DirectSearchLimit) {
        return directBestOffset(base, fit, cancellation);
    }
    return fftBestOffset(base, fit, cancellation);
}

BatchResult findBestSeries(
    const QVector<Series> &series,
    const std::atomic_bool *cancellation)
{
    BatchResult result;
    for (const Series &input : series) {
        if (isCancelled(cancellation)) {
            result = BatchResult();
            result.cancelled = true;
            return result;
        }

        const Result candidate =
            findBestOffset(input.base, input.fit, cancellation);
        if (candidate.cancelled) {
            result = BatchResult();
            result.cancelled = true;
            return result;
        }
        if (candidate.valid && candidate.rSquared > result.rSquared) {
            result.valid = true;
            result.seriesKey = input.key;
            result.offset = candidate.offset;
            result.rSquared = candidate.rSquared;
        }
    }
    return result;
}

Runner::Runner(QObject *parent)
    : QObject(parent)
{
    connect(&watcher_, &QFutureWatcher<BatchResult>::finished,
            this, &Runner::futureFinished);
}

Runner::~Runner()
{
    shuttingDown_ = true;
    disconnect(&watcher_, nullptr, this, nullptr);
    cancel();
    if (watcher_.isRunning()) watcher_.waitForFinished();
}

bool Runner::start(QVector<Series> series)
{
    if (shuttingDown_ || watcher_.isRunning()) return false;

    cancellation_ = std::make_shared<std::atomic_bool>(false);
    const std::shared_ptr<std::atomic_bool> cancellation = cancellation_;
    QFuture<BatchResult> future = QtConcurrent::run(
        [series = std::move(series), cancellation]() {
            return findBestSeries(series, cancellation.get());
        });
    watcher_.setFuture(future);
    return true;
}

void Runner::cancel()
{
    if (cancellation_) {
        cancellation_->store(true, std::memory_order_relaxed);
    }
}

bool Runner::isRunning() const
{
    return watcher_.isRunning();
}

void Runner::futureFinished()
{
    BatchResult result;
    try {
        result = watcher_.result();
    } catch (const std::exception &) {
        result = BatchResult();
    } catch (...) {
        result = BatchResult();
    }

    if (cancellation_ &&
        cancellation_->load(std::memory_order_relaxed)) {
        result = BatchResult();
        result.cancelled = true;
    }
    cancellation_.reset();

    if (!shuttingDown_) emit completed(result);
}

} // namespace MergeActivityAlignment
