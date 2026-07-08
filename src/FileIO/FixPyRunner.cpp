#include <QApplication>
#include <QtConcurrent>

#include "FixPyRunner.h"
#include "PythonEmbed.h"
#include "RideFileCommand.h"

FixPyRunner::FixPyRunner(Context *context, RideFile *rideFile, RideItem *rideItem, bool useNewThread)
    : context(context), rideFile(rideFile), rideItem(rideItem), useNewThread(useNewThread)
{
}

int FixPyRunner::run(QString source, QString scriptKey, QString &errText)
{
    if (source.isEmpty()) {
        return 1;
    }

    // hourglass .. for long running ones this helps user know its busy
    QApplication::setOverrideCursor(Qt::WaitCursor);

    QString line = source;
    int result = 0;

    FixPyRunParams params;
    params.context = context;
    params.rideFile = rideFile;
    params.rideItem = rideItem;

    try {

        // replace $$ with script identifier (to avoid shared data)
        line = line.replace("$$", scriptKey);

        // run it
        params.script = QString(line);

        if (useNewThread) {
            QFutureWatcher<void> watcher;
            QFuture<void> f = QtConcurrent::run(execScript, &params);

            // wait for it to finish -- remember ESC can be pressed to cancel
            watcher.setFuture(f);
            QEventLoop loop;
            connect(&watcher, SIGNAL(finished()), &loop, SLOT(quit()));
            loop.exec();
        } else {
            execScript(&params);
        }

        // output on console
        if (!params.messages.isEmpty()) {
            errText = params.messages.join("\n");
        }
        if (!params.error.isEmpty()) {
            errText = params.error;
            result = 2;
        }

    } catch(std::exception& ex) {
        errText = QString("\n%1\n%2").arg(QString(ex.what())).arg(params.messages.join(""));
        result = 2;
    } catch(...) {
        errText = QString("\nerror: general exception.\n%1").arg(params.messages.join(""));
        result = 3;
    }

    // reset cursor
    QApplication::restoreOverrideCursor();

    return result;
}

void FixPyRunner::execScript(FixPyRunParams *params)
{
    QList<RideFile *> editedRideFiles;
    const PythonRunResult result = python->runline(
        ScriptContext(
            params->context, params->rideFile, params->rideItem, false,
            false, &editedRideFiles),
        params->script);
    params->messages = result.messages;
    params->error = result.error;

    // finish up commands on edited rides
    foreach (RideFile *f, editedRideFiles) {
        f->command->endLUW();
    }
}
