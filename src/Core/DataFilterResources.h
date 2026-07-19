/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_DATAFILTERRESOURCES_H
#define GC_DATAFILTERRESOURCES_H

#include <QList>
#include <QtAlgorithms>

#include <gsl/gsl_rng.h>

template<typename Model>
class DataFilterResourceOwner
{
public:
    DataFilterResourceOwner() = default;
    ~DataFilterResourceOwner()
    {
        reset();
    }

    DataFilterResourceOwner(const DataFilterResourceOwner &) = delete;
    DataFilterResourceOwner &operator=(
        const DataFilterResourceOwner &) = delete;

    void addModel(Model *model)
    {
        if (model) models_.append(model);
    }

    const QList<Model*> &models() const
    {
        return models_;
    }

    void setRandomGenerator(gsl_rng *randomGenerator)
    {
        if (randomGenerator_ == randomGenerator) return;
        if (randomGenerator_) gsl_rng_free(randomGenerator_);
        randomGenerator_ = randomGenerator;
    }

    gsl_rng *randomGenerator() const
    {
        return randomGenerator_;
    }

private:
    void reset()
    {
        qDeleteAll(models_);
        models_.clear();
        if (randomGenerator_) {
            gsl_rng_free(randomGenerator_);
            randomGenerator_ = nullptr;
        }
    }

    QList<Model*> models_;
    gsl_rng *randomGenerator_ = nullptr;
};

#endif // GC_DATAFILTERRESOURCES_H
