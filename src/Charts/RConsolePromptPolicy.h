/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef GC_RCONSOLEPROMPTPOLICY_H
#define GC_RCONSOLEPROMPTPOLICY_H

#include <QString>

class RConsolePromptPolicy final
{
public:
    static bool hasPromptPrefix(const QString &lastBlock)
    {
        return lastBlock.startsWith(QStringLiteral("> "))
            || lastBlock.startsWith(QStringLiteral(">>"));
    }

    static bool shouldAppendDeferred(const QString &lastBlock)
    {
        return !hasPromptPrefix(lastBlock);
    }

    static QString prompt(bool continuation)
    {
        return continuation ? QStringLiteral(">>") : QStringLiteral("> ");
    }
};

#endif
