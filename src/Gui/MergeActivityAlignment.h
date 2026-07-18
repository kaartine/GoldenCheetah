/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef GC_MERGEACTIVITYALIGNMENT_H
#define GC_MERGEACTIVITYALIGNMENT_H

#include <QFutureWatcher>
#include <QObject>
#include <QVector>

#include <atomic>
#include <memory>

namespace MergeActivityAlignment {

struct Result
{
    bool valid = false;
    bool cancelled = false;
    int offset = 0;
    double rSquared = 0.0;
};

struct Series
{
    int key = 0;
    QVector<double> base;
    QVector<double> fit;
};

struct BatchResult
{
    bool valid = false;
    bool cancelled = false;
    int seriesKey = 0;
    int offset = 0;
    double rSquared = 0.0;
};

Result findBestOffset(
    const QVector<double> &base,
    const QVector<double> &fit,
    const std::atomic_bool *cancellation = nullptr);

BatchResult findBestSeries(
    const QVector<Series> &series,
    const std::atomic_bool *cancellation = nullptr);

class Runner final : public QObject
{
    Q_OBJECT

public:
    explicit Runner(QObject *parent = nullptr);
    ~Runner() override;

    Runner(const Runner &) = delete;
    Runner &operator=(const Runner &) = delete;

    bool start(QVector<Series> series);
    void cancel();
    bool isRunning() const;

signals:
    void completed(const MergeActivityAlignment::BatchResult &result);

private:
    void futureFinished();

    QFutureWatcher<BatchResult> watcher_;
    std::shared_ptr<std::atomic_bool> cancellation_;
    bool shuttingDown_ = false;
};

} // namespace MergeActivityAlignment

Q_DECLARE_METATYPE(MergeActivityAlignment::BatchResult)

#endif
