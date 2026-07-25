/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_FIXGPSSMOOTHINGSAFETY_H
#define GC_FIXGPSSMOOTHINGSAFETY_H

#include <cstddef>
#include <vector>

namespace FixGPSSmoothingSafety {

using SourceIndexes = std::vector<std::size_t>;

inline bool hasValidSourceIndexes(std::size_t sourceCount,
                                  const SourceIndexes &sourceIndexes)
{
    if (sourceIndexes.empty()) {
        return false;
    }

    std::size_t previous = 0;
    bool havePrevious = false;
    for (const std::size_t sourceIndex : sourceIndexes) {
        if (sourceIndex >= sourceCount ||
            (havePrevious && sourceIndex <= previous)) {
            return false;
        }
        previous = sourceIndex;
        havePrevious = true;
    }
    return true;
}

inline bool hasUsableSplineInput(std::size_t controlCount, unsigned degree)
{
    return controlCount >= 4 && degree >= 3;
}

inline bool hasAlignedControlOutput(std::size_t controlCount,
                                    std::size_t outputCount)
{
    return controlCount > 0 && controlCount == outputCount;
}

template<typename Control, typename Builder>
bool gatherControls(std::size_t sourceCount,
                    std::vector<Control> &controls,
                    SourceIndexes &sourceIndexes,
                    Builder &&builder)
{
    controls.clear();
    sourceIndexes.clear();
    controls.reserve(sourceCount);
    sourceIndexes.reserve(sourceCount);

    for (std::size_t sourceIndex = 0;
         sourceIndex < sourceCount;
         ++sourceIndex) {
        Control control{};
        if (builder(sourceIndex, control)) {
            controls.push_back(control);
            sourceIndexes.push_back(sourceIndex);
        }
    }

    return !controls.empty();
}

template<typename Input, typename Output, typename Builder>
bool gatherMappedControls(std::size_t sourceCount,
                          const SourceIndexes &inputSourceIndexes,
                          const std::vector<Input> &inputs,
                          std::vector<Output> &outputs,
                          SourceIndexes &outputSourceIndexes,
                          Builder &&builder)
{
    outputs.clear();
    outputSourceIndexes.clear();

    if (inputSourceIndexes.size() != inputs.size() ||
        !hasValidSourceIndexes(sourceCount, inputSourceIndexes)) {
        return false;
    }

    outputs.reserve(inputs.size());
    outputSourceIndexes.reserve(inputs.size());
    for (std::size_t inputIndex = 0;
         inputIndex < inputs.size();
         ++inputIndex) {
        Output output{};
        const std::size_t sourceIndex = inputSourceIndexes[inputIndex];
        if (builder(sourceIndex, inputs[inputIndex], output)) {
            outputs.push_back(output);
            outputSourceIndexes.push_back(sourceIndex);
        }
    }

    return !outputs.empty();
}

template<typename Output, typename Apply>
bool applyMappedOutputs(std::size_t sourceCount,
                        const SourceIndexes &sourceIndexes,
                        const std::vector<Output> &outputs,
                        Apply &&apply)
{
    if (sourceIndexes.size() != outputs.size() ||
        !hasValidSourceIndexes(sourceCount, sourceIndexes)) {
        return false;
    }

    for (std::size_t outputIndex = 0;
         outputIndex < outputs.size();
         ++outputIndex) {
        apply(sourceIndexes[outputIndex], outputs[outputIndex]);
    }
    return true;
}

} // namespace FixGPSSmoothingSafety

#endif
