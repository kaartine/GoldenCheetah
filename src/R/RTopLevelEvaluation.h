/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_R_TOP_LEVEL_EVALUATION_H
#define GC_R_TOP_LEVEL_EVALUATION_H

#include <R_ext/Parse.h>
#include <Rinternals.h>

struct RTopLevelEvaluationData {
    const char *command;
    bool verbose;
    void (*printValue)(SEXP);
    ParseStatus status;
    SEXP answer;
    int errorOccurred;
    bool completed;
};

inline void parseAndEvaluateAtRTopLevel(void *opaque) noexcept
{
    RTopLevelEvaluationData *data =
        static_cast<RTopLevelEvaluationData *>(opaque);
    SEXP command = PROTECT(Rf_allocVector(STRSXP, 1));
    SET_STRING_ELT(command, 0, Rf_mkChar(data->command));
    SEXP expressions = PROTECT(
        R_ParseVector(command, -1, &data->status, R_NilValue));

    if (data->status == PARSE_OK) {
        for (int index = 0; index < Rf_length(expressions); ++index) {
            data->answer = R_tryEval(
                VECTOR_ELT(expressions, index), R_GlobalEnv,
                &data->errorOccurred);
            if (data->errorOccurred) break;
            if (data->verbose) {
                PROTECT(data->answer);
                data->printValue(data->answer);
                UNPROTECT(1);
            }
        }
    }

    UNPROTECT(2);
    data->completed = true;
}

#endif
