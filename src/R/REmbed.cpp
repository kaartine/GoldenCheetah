/*
 * Copyright (c) 2016 Mark Liversedge (liversedge@gmail.com)
 *
 * Additionally, for the original source used as a basis for this (RInside.cpp)
 * Released under the same GNU public license.
 *
 * Copyright (C) 2009         Dirk Eddelbuettel
 * Copyright (C) 2010 - 2012  Dirk Eddelbuettel and Romain Francois
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "REmbed.h"
#include "RTool.h"
#include "RTopLevelBoundary.h"
#include "RTopLevelEvaluation.h"
#include "Settings.h"
#include <stdexcept>

#include <QMessageBox>

static const char *name = "GoldenCheetah";

namespace {

struct EmbeddedSetupData {
    bool interactive;
    bool completed;
};

struct EmbeddedShutdownData {
    bool completed;
};

void initializeEmbeddedRuntime(void *opaque) noexcept
{
    EmbeddedSetupData *data = static_cast<EmbeddedSetupData *>(opaque);
    R_ReplDLLinit();

    structRstart start;
    R_DefParams(&start);
#ifdef WIN32
    start.rhome = getenv("R_HOME");
    start.home = getRUser();
    start.CharacterMode = LinkDLL;
    start.ReadConsole = &RTool::R_ReadConsoleWin;
    start.WriteConsole = &RTool::R_WriteConsole;
    start.WriteConsoleEx = &RTool::R_WriteConsoleEx;
    start.CallBack = &RTool::R_ProcessEvents;
    start.ShowMessage = &RTool::R_ShowMessage;
    start.YesNoCancel = &RTool::R_YesNoCancel;
    start.Busy = &RTool::R_Busy;
#endif
    start.R_Interactive = static_cast<Rboolean>(data->interactive);
    R_SetParams(&start);
    data->completed = true;
}

void shutdownEmbeddedRuntime(void *opaque) noexcept
{
    EmbeddedShutdownData *data =
        static_cast<EmbeddedShutdownData *>(opaque);
    Rf_endEmbeddedR(0);
    data->completed = true;
}

}

// no setenv on windows
#if WIN32
int setenv(QString name, QString value, bool overwrite)
{
    int errcode = 0;
    if(!overwrite) {
        size_t envsize = 0;
        errcode = getenv_s(&envsize, NULL, 0, name.toLatin1().constData());
        if(errcode || envsize) return errcode;
    }

    // make the update
    return _putenv_s(name.toLatin1().constData(), value.toLatin1().constData());
}
#endif

REmbed::~REmbed()
{
    // Deliberately no finalization here. Safe shutdown needs every callback,
    // device, and interpreter user drained on the initialization thread; that
    // ordering is not yet proven. Initialized wrappers are retained by their
    // process-lifetime owner until the explicit shutdown work is complete.
}

// Windows API defines Read and Write Console macros
// You think they'd realise by now that generic terms
// should be avoided. But no, blissfully unaware that
// there are millions of lines of code out there they
// can wantanly break. Fuckwits.
#ifdef WIN32
#ifdef ReadConsole
#undef ReadConsole
#undef WriteConsole
#endif
#endif

REmbed::REmbed(const bool verbose, const bool interactive)
    : verbose(verbose), interactive(interactive)
{
    loaded = false;
}

void
REmbed::initialize()
{
    if (initializationState_ != InitializationState::NotStarted) return;

    // need to load the library
    RLibrary rlib;
    if (!rlib.load()) {

        // disable for future
        appsettings->setValue(GC_EMBED_R, false);
        rlib.errors.append("\nR has now been disabled in options.");

        QMessageBox msg(QMessageBox::Information,
                    "Failed to load R library",
                    rlib.errors.join("\n"));
        msg.exec();
        return;
    }

    // we need to tell embedded R where to work
    QString envR_HOME(getenv("R_HOME"));
    QString configR_HOME = appsettings->value(NULL,GC_R_HOME,"").toString();
    if (envR_HOME == "") {
        if (configR_HOME == "") {
            qDebug()<<"R HOME not set, R disabled";
            return;
        } else {
            setenv("R_HOME", configR_HOME.toLatin1().constData(), true);
        }
    }
    // fire up R
    const char *R_argv[] = {name, "--gui=none", "--no-save",
                            "--no-readline", "--silent", "--vanilla", "--slave"};
    int R_argc = sizeof(R_argv) / sizeof(R_argv[0]);
    const int initResult = Rf_initEmbeddedR(R_argc, (char**)R_argv);
    if (initResult < 0) return;
    initializationState_ = InitializationState::InterpreterInitialized;

    EmbeddedSetupData setup{interactive, false};
    if (!executeAtRTopLevel(initializeEmbeddedRuntime, &setup)
        || !setup.completed) return;

    loaded = true;
    initializationState_ = InitializationState::Ready;
}

bool
REmbed::shutdown()
{
    if (shutdownAttempted_) return false;
    shutdownAttempted_ = true;
    if (initializationState_ == InitializationState::NotStarted) return true;
    if (initializationState_ != InitializationState::InterpreterInitialized
        && initializationState_ != InitializationState::Ready) return false;

    initializationState_ = InitializationState::ShuttingDown;
    EmbeddedShutdownData shutdown{false};
    if (!executeAtRTopLevel(shutdownEmbeddedRuntime, &shutdown)
        || !shutdown.completed) {
        loaded = false;
        initializationState_ = InitializationState::ShutdownFailed;
        return false;
    }

    loaded = false;
    initializationState_ = InitializationState::Finalized;
    return true;
}

// this is a non-throwing version returning an error code
int REmbed::parseEval(QString line, SEXP & ans) {
    program << line;
    const QByteArray command = program.join(" ").toUtf8();
    RTopLevelEvaluationData execution{
        command.constData(), verbose, Rf_PrintValue,
        PARSE_NULL, R_NilValue, 0, false};
    if (!executeAtRTopLevel(parseAndEvaluateAtRTopLevel, &execution)
        || !execution.completed) {
        program.clear();
        return 1;
    }
    ans = execution.answer;

    switch (execution.status){
    case PARSE_OK:
        if (execution.errorOccurred) {
            if (verbose) qWarning() << name << "error evaluating R code";
            program.clear();
            return 1;
        }
        program.clear();
        break;
    case PARSE_INCOMPLETE:
        // need to read another line
        break;
    case PARSE_NULL:
        if (verbose) qWarning() << name << "R parse status is null";
        program.clear();
        return 1;
    case PARSE_ERROR:
        if (verbose) qWarning() << name << "R parse error:" << line;
        program.clear();
        return 1;
    case PARSE_EOF:
        if (verbose) qWarning() << name << "R parse status is EOF";
        break;
    default:
        if (verbose) qWarning() << name << "undocumented R parse status"
                                << static_cast<int>(execution.status);
        program.clear();
        return 1;
    }
    return 0;
}

void REmbed::parseEvalQ(QString line) {
    SEXP ans;
    int rc = parseEval(line, ans);
    if (rc != 0) {
        throw std::runtime_error(std::string("Error evaluating: ") + line.toStdString());
    }
}

void REmbed::parseEvalQNT(QString line) {
    SEXP ans;
    parseEval(line, ans);
}

SEXP REmbed::parseEval(QString line) {
    SEXP ans;
    int rc = parseEval(line, ans);
    if (rc != 0) {
        throw std::runtime_error(std::string("Error evaluating: ") + line.toStdString());
    }
    return ans;
}

SEXP REmbed::parseEvalNT(QString line) {
    SEXP ans;
    parseEval(line, ans);
    return ans;
}
