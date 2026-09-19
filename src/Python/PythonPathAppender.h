/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_PYTHONPATHAPPENDER_H
#define GC_PYTHONPATHAPPENDER_H

#include <functional>

class PythonPathAppender final
{
public:
    enum class Result {
        Rejected,
        Appended,
        MissingPath,
        InvalidPath,
        AllocationFailed,
        AppendFailed
    };

    struct Hooks {
        std::function<void *(void *)> getPath;
        std::function<bool(void *)> isList;
        std::function<void *()> createEntry;
        std::function<bool(void *, void *)> append;
        std::function<void(void *)> release;
    };

    static Result append(void *sysModule, const Hooks &hooks)
    {
        if (!sysModule || !hooks.getPath || !hooks.isList
            || !hooks.createEntry || !hooks.append || !hooks.release) {
            return Result::Rejected;
        }

        void *path = hooks.getPath(sysModule);
        if (!path) return Result::MissingPath;
        if (!hooks.isList(path)) {
            hooks.release(path);
            return Result::InvalidPath;
        }

        void *entry = hooks.createEntry();
        if (!entry) {
            hooks.release(path);
            return Result::AllocationFailed;
        }

        const bool appended = hooks.append(path, entry);
        hooks.release(entry);
        hooks.release(path);
        return appended ? Result::Appended : Result::AppendFailed;
    }
};

#endif
