/*
 * Copyright (c) 2017 Mark Liversedge (liversedge@gmail.com)
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

#include "PythonChart.h"
#include "PythonEmbed.h"
#include "PythonSyntax.h"

#include "Colors.h"
#include "AbstractView.h"
#include "RideFileCommand.h"
#include "HelpWhatsThis.h"

#include <QPointer>
#include <QWebEngineSettings>

#include <utility>

// always pull in after all QT headers
#ifdef slots
#undef slots
#endif
#include <Python.h>

// unique identifier for each chart
static int id=0;

namespace {

void snapshotScriptContext(
    ScriptContext &scriptContext,
    Context *context,
    PythonChart *chart)
{
    scriptContext.chartCommandsEnabled = chart != nullptr;
    if (context) {
        scriptContext.contextFiltered = context->isfiltered;
        scriptContext.contextFilters = context->filters;
        scriptContext.homeFiltered = context->ishomefiltered;
        scriptContext.homeFilters = context->homeFilters;
    }
    if (chart && chart->myPerspective) {
        scriptContext.perspectiveFiltered =
                chart->myPerspective->isFiltered();
        scriptContext.perspectiveFilters =
                chart->myPerspective->filterlist(
                    DateRange(
                        QDate(1, 1, 1970),
                        QDate(31, 12, 3000)));
    }
}

}

PythonConsole::PythonConsole(Context *context, PythonHost *pythonHost, QWidget *parent)
    : QTextEdit(parent)
    , context(context), localEchoEnabled(true), pythonHost(pythonHost)
{
    setSizePolicy(QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding));
    setFrameStyle(QFrame::NoFrame);
    setAcceptRichText(false);
    document()->setMaximumBlockCount(512); // lets not get carried away!
    putData(GColor(CPLOTMARKER), QString(tr("Python Console (%1)").arg(python->version)));
    putData(GCColor::invertColor(GColor(CPLOTBACKGROUND)), "\n>>> ");

    connect(context, SIGNAL(configChanged(qint32)), this, SLOT(configChanged(qint32)));
    connect(context, SIGNAL(rMessage(QString)), this, SLOT(rMessage(QString)));

    // history position
    hpos=0;

    // set unique runtimeid name
    chartid = QString("gc%1").arg(id++);

    configChanged(0);
}

void
PythonConsole::configChanged(qint32)
{
    QFont courier("Courier", QFont().pointSize());
    setFont(courier);
    QPalette p = palette();
    p.setColor(QPalette::Base, GColor(CPLOTBACKGROUND));
    p.setColor(QPalette::Text, GCColor::invertColor(GColor(CPLOTBACKGROUND)));
    setPalette(p);
    setStyleSheet(AbstractView::ourStyleSheet());
}

void
PythonConsole::rMessage(QString x)
{
    putData(GColor(CPLOTMARKER), x);
}

void PythonConsole::putData(QColor color, QString string)
{
    // change color...
    setTextColor(color);
    putData(string);
}


void PythonConsole::putData(QString data)
{
    insertPlainText(QString(data));

    QScrollBar *bar = verticalScrollBar();
    bar->setValue(bar->maximum());
}

void PythonConsole::setLocalEchoEnabled(bool set)
{
    localEchoEnabled = set;
}

void PythonConsole::keyPressEvent(QKeyEvent *e)
{
    switch (e->key()) {
    case Qt::Key_Up:
        if (hpos) {
            hpos--;
            setCurrentLine(history[hpos]);
        }
        break;

    case Qt::Key_Down:
        if (hpos < history.count()-1) {
            hpos++;
            setCurrentLine(history[hpos]);
        }
        break;

    // you can always go to the right
    case Qt::Key_Right:
        QTextEdit::keyPressEvent(e);
        break;

    // you can only delete or move left from past first character
    case Qt::Key_Left:
    case Qt::Key_Backspace:
        if (textCursor().positionInBlock() > promptStartIndex) QTextEdit::keyPressEvent(e);
        break;

    case Qt::Key_Home:
        {
            QTextCursor moveHome = textCursor();
            moveHome.setPosition(textCursor().block().position() + promptStartIndex, QTextCursor::MoveAnchor);
            setTextCursor(moveHome);
        }
        break;

    case Qt::Key_Escape: // R typically uses ESC to cancel
    case Qt::Key_C:
        {

            Qt::KeyboardModifiers kmod = static_cast<QInputEvent*>(e)->modifiers();
            bool ctrl = (kmod & Qt::ControlModifier) != 0;

            if (e->key() == Qt::Key_Escape || ctrl) {

                // ESC or ^C needs to clear program and go to next line
                python->program.clear();

                QTextCursor move = textCursor();
                move.movePosition(QTextCursor::End);
                setTextCursor(move);

                // new prompt
                putData("\n");
                putData(GCColor::invertColor(GColor(CPLOTBACKGROUND)), ">>> ");

            } else {
                // normal C just do the usual
                if (localEchoEnabled) QTextEdit::keyPressEvent(e);
            }
        }
        break;

    case Qt::Key_Enter:
    case Qt::Key_Return:
    {
        QTextCursor move = textCursor();
        move.movePosition(QTextCursor::End);
        setTextCursor(move);

        QString line = currentLine();
        if (line.length() > promptStartIndex) line = line.mid(promptStartIndex, line.length() - promptStartIndex);
        else line = "";

        putData("\n");

        if (line != "") {

            history << line;
            hpos = history.count();

            // lets run it
            //qDebug()<<"RUN:" << line;

            PythonRunResult runResult;
            QPointer<PythonChart> chartGuard(pythonHost->chart());
            try {

                // replace $$ with chart identifier (to avoid shared data)
                line = line.replace("$$", chartid);

                bool readOnly = pythonHost->readOnly();
                QList<RideFile *> editedRideFiles;
                ScriptContext scriptContext(
                    context, nullptr, nullptr, true, readOnly,
                    &editedRideFiles);
                snapshotScriptContext(
                    scriptContext, context, chartGuard.data());
                runResult = python->runline(scriptContext, line);

                if (chartGuard && !runResult.cancelled) {
                    for (const auto &command : runResult.chartCommands) {
                        command(chartGuard.data());
                    }
                }

                // finish up commands on edited rides
                foreach (RideFile *f, editedRideFiles) {
                    f->command->endLUW();
                }

                // the run command should result in some messages being generated
                if (!runResult.messages.isEmpty()) {
                    putData(
                        runResult.error.isEmpty()
                            ? GColor(CPLOTMARKER) : QColor(Qt::red),
                        runResult.messages.join(""));
                } else if (!runResult.error.isEmpty()) {
                    putData(QColor(Qt::red), runResult.error + "\n");
                }

            } catch(std::exception& ex) {

                putData(QColor(Qt::red), QString("%1\n").arg(QString(ex.what())));
                putData(QColor(Qt::red), runResult.messages.join("\n"));

            } catch(...) {

                putData(QColor(Qt::red), "error: general exception.\n");
                putData(QColor(Qt::red), runResult.messages.join("\n"));

            }
        }

        // prompt ">"
        putData(GCColor::invertColor(GColor(CPLOTBACKGROUND)), ">>> ");
    }
    break;

    default:
        if (localEchoEnabled) QTextEdit::keyPressEvent(e);
        emit getData(e->text().toLocal8Bit());
    }

    // if we edit or anything reset the history position
    if (e->key()!= Qt::Key_Up && e->key()!=Qt::Key_Down) hpos = history.count();
}

void
PythonConsole::setCurrentLine(QString p)
{
    QTextCursor select = textCursor();

    select.select(QTextCursor::LineUnderCursor);
    select.removeSelectedText();
    putData(GCColor::invertColor(GColor(CPLOTBACKGROUND)), ">>> ");
    putData(p);
}

QString
PythonConsole::currentLine()
{
    return textCursor().block().text().trimmed();
}

void PythonConsole::mousePressEvent(QMouseEvent *e)
{
    Q_UNUSED(e)
    setFocus();
}

void PythonConsole::mouseDoubleClickEvent(QMouseEvent *e)
{
    Q_UNUSED(e)
}

void PythonConsole::contextMenuEvent(QContextMenuEvent *e)
{
    Q_UNUSED(e)
}

PythonChart::PythonChart(Context *context, bool ridesummary)
    : GcChartWindow(context)
    , context(context)
    , ridesummary(ridesummary)
    , syntax(nullptr)
    , owner(
        {
            [this]() { return prepareRun(); },
            [this](bool busy) { setRunBusy(busy); },
            [this](PythonRunResult result) {
                applyRunResult(std::move(result));
            }
        },
        execScript,
        [](quint64 token) {
            if (python) python->cancel(token);
        })
{
    HelpWhatsThis *helpContents = new HelpWhatsThis(this);
    this->setWhatsThis(helpContents->getWhatsThisText(HelpWhatsThis::Chart_Python));

    // controls widget
    QWidget *c = new QWidget;
    HelpWhatsThis *helpConfig = new HelpWhatsThis(c);
    c->setWhatsThis(helpConfig->getWhatsThisText(HelpWhatsThis::Chart_Python));
    setControls(c);

    // settings
    QVBoxLayout *clv = new QVBoxLayout(c);
    web = new QCheckBox(tr("Web charting"), this);
    clv->addWidget(web);

    // sert no render widget
    canvas=NULL;
    plot=NULL;

    QVBoxLayout *mainLayout = new QVBoxLayout;
    mainLayout->setSpacing(0);
    mainLayout->setContentsMargins(0,0,0,0);
    setChartLayout(mainLayout);

    // if we failed to startup embedded R properly
    // then disable the PythonConsole altogether.
    if (python) {
        showCon = new QCheckBox(tr("Show Console"), this);
        showCon->setChecked(true);
        clv->addWidget(showCon);
        clv->addStretch();

        leftsplitter = new QSplitter(Qt::Vertical, this);
        leftsplitter->setSizePolicy(QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding));
        leftsplitter->setHandleWidth(1);

        // LHS
        script = new  QTextEdit(this);
        script->setSizePolicy(QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding));
        script->setFrameStyle(QFrame::NoFrame);
        script->setAcceptRichText(false);
        QFont courier("Courier", QFont().pointSize());
        script->setFont(courier);
        QPalette p = palette();
        p.setColor(QPalette::Base, GColor(CPLOTBACKGROUND));
        p.setColor(QPalette::Text, GCColor::invertColor(GColor(CPLOTBACKGROUND)));
        script->setPalette(p);
        script->setStyleSheet(AbstractView::ourStyleSheet());

        // syntax highlighter
        setScript("##\n## Python program will run on selection.\n##\n");

        leftsplitter->addWidget(script);
        console = new PythonConsole(context, this, this);
        console->setSizePolicy(QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding));
        leftsplitter->addWidget(console);

        splitter = new QSplitter(Qt::Horizontal, this);
        splitter->setSizePolicy(QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding));
        splitter->setHandleWidth(1);
        mainLayout->addWidget(splitter);

        splitter->addWidget(leftsplitter);

        // for Chart or webpage
        render = new QWidget(this);
        renderlayout = new QVBoxLayout(render);
        splitter->addWidget(render);

        // make splitter reasonable
        QList<int> sizes;
        sizes << 300 << 500;
        splitter->setSizes(sizes);

        // passing data across python and gui threads
        connect(this, SIGNAL(setUrl(QUrl)), this, SLOT(webpage(QUrl)));

        if (ridesummary) {
            connect(this, SIGNAL(rideItemChanged(RideItem*)), this, SLOT(runScript()));
            connect(context, SIGNAL(rideChanged(RideItem*)), this, SLOT(runScript()));

            // refresh when comparing
            connect(context, SIGNAL(compareIntervalsStateChanged(bool)), this, SLOT(runScript()));
            connect(context, SIGNAL(compareIntervalsChanged()), this, SLOT(runScript()));

            // refresh when intervals are selected
            connect(context, SIGNAL(intervalSelected()), this, SLOT(runScript()));

        } else {
            connect(this, SIGNAL(dateRangeChanged(DateRange)), this, SLOT(runScript()));

            // refresh when comparing
            connect(context, SIGNAL(compareDateRangesStateChanged(bool)), this, SLOT(runScript()));
            connect(context, SIGNAL(compareDateRangesChanged()), this, SLOT(runScript()));
        }

        // we apply BOTH filters, so update when either change
        connect(context, SIGNAL(filterChanged()), this, SLOT(runScript()));
        connect(context, SIGNAL(homeFilterChanged()), this, SLOT(runScript()));

        // reveal controls
        connect(showCon, SIGNAL(stateChanged(int)), this, SLOT(showConChanged(int)));
        connect(web, SIGNAL(stateChanged(int)), this, SLOT(showWebChanged(int)));

        // config changes
        connect(context, SIGNAL(configChanged(qint32)), this, SLOT(configChanged(qint32)));
        configChanged(CONFIG_APPEARANCE);

        // filter ESC so we can stop scripts
        if (qApp) qApp->installEventFilter(this);
    } else {
        // not starting
        noPython = new QLabel(tr("Warning: Python is disabled"), this);
        clv->addWidget(noPython);
        clv->addStretch();

        script = NULL;
        splitter = NULL;
        console = NULL;
        canvas = NULL;
        showCon = NULL;
        leftsplitter = NULL;
    }
    web->setChecked(true);
}

PythonChart::~PythonChart()
{
    if (qApp) qApp->removeEventFilter(this);
    owner.shutdown();
    if (canvas) delete canvas->page();
}

// switch between rendering to a web page and rendering to a chart page
void
PythonChart::setWeb(bool x)
{
    // check python was loaded
    if (python == NULL || python->loaded == false) return;

    // toggle the use of a web chart or a qt chart for rendering the data
    if (x && canvas==NULL) {

        // delete the chart view if exists
        if (plot) {
            renderlayout->removeWidget(plot);
            delete plot; // deletes associated chart too
            plot = NULL;
        }

        // setup the canvas
        canvas = new QWebEngineView(this);
        canvas->setUrl(QUrl());
        canvas->setContentsMargins(0,0,0,0);
        canvas->setZoomFactor(dpiXFactor);
        canvas->setSizePolicy(QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding));
        // stop stealing focus!
        canvas->settings()->setAttribute(QWebEngineSettings::FocusOnNavigationEnabled, false);
        canvas->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
        canvas->setAcceptDrops(false);
        renderlayout->insertWidget(0, canvas);
    }

    if (!x && plot==NULL) {

        // delete the canvas if exists
        if (canvas) {
            renderlayout->removeWidget(canvas);
            delete canvas;
            canvas = NULL;
        }

        // setup the chart
        plot = new GenericChart(NULL,context); //XXX todo: null to avoid crash on close...
        renderlayout->insertWidget(0,plot);

        // signals to update it
        connect(this, SIGNAL(emitChart(QString,int,bool,int,bool,int)), plot, SLOT(initialiseChart(QString,int,bool,int,bool,int)));
        connect(this, SIGNAL(emitCurve(QString,QVector<double>,QVector<double>,QStringList, QString,QString,QStringList,QStringList,int,int,int,QString,int,bool,bool,bool,bool)),
                plot,   SLOT( addCurve(QString,QVector<double>,QVector<double>,QStringList,QString,QString,QStringList,QStringList,int,int,int,QString,int,bool,bool,bool,bool)));
        connect(this, SIGNAL(emitAxis(QString,bool,int,double,double,int,QString,QString,bool,QStringList)),
                plot,   SLOT(configureAxis(QString,bool,int,double,double,int,QString,QString,bool,QStringList)));
        //connect(this,SIGNAL(emitAnnotation(QString,QStringList)), plot,  SLOT(annotateLabel(QString,QStringList))); // XXX fixme

    }

    // set the check state!
    web->setChecked(x);

    // config changed...
    configChanged(0);
}

bool
PythonChart::eventFilter(QObject *watched, QEvent *e)
{
    QWidget *target = qobject_cast<QWidget *>(watched);
    const bool belongsToChart =
            target && (target == this || isAncestorOf(target));
    if (owner.active() && belongsToChart
        && e->type() == QEvent::KeyPress) {
        if (static_cast<QKeyEvent *>(e)->key() == Qt::Key_Escape) {
            owner.cancelCurrent();
        }
        return true;
    }

    // on resize event scale the display
    if (e->type() == QEvent::Resize) {
        //canvas->fitInView(canvas->sceneRect(), Qt::KeepAspectRatio);
    }

    return false;
}

void
PythonChart::configChanged(qint32)
{
    QColor bgcolor = !ridesummary ? GColor(CTRENDPLOTBACKGROUND) : GColor(CPLOTBACKGROUND);
    setProperty("color", bgcolor);
    if (plot) plot->setBackgroundColor(bgcolor);

    // tinted palette for headings etc
    QPalette palette;
    palette.setBrush(QPalette::Window, QBrush(GColor(CPLOTBACKGROUND)));
    palette.setColor(QPalette::WindowText, GColor(CPLOTMARKER));
    palette.setColor(QPalette::Text, GColor(CPLOTMARKER));
    palette.setColor(QPalette::Base, GCColor::alternateColor(GColor(CPLOTBACKGROUND)));
    setPalette(palette);
    script->setPalette(palette);
    script->setStyleSheet(AbstractView::ourStyleSheet());

    // refresh highlighter
    if (syntax) delete syntax;
    syntax = new PythonSyntax(script->document(), GCColor::isPaletteDark(palette));
    runScript();
}

void
PythonChart::setConsole(bool show)
{
    if (showCon) showCon->setChecked(show);
}

void
PythonChart::showConChanged(int state)
{
    if (leftsplitter) leftsplitter->setVisible(state);
}

void
PythonChart::showWebChanged(int state)
{
    setWeb(state);
}

QString
PythonChart::getScript() const
{
    if (python && script) return script->toPlainText();
    else return text;
}
void
PythonChart::setScript(QString string)
{
    if (python && script) {
        script->setText(string);
    }
    text = string;
}

QString
PythonChart::getState() const
{
    //XXX FIXME
    //if (python && splitter)  return QString(splitter->saveState());
    //else return "";
    return "";
}

void
PythonChart::setState(QString)
{
    //XXX FIXME
    //if (python && splitter && b != "") splitter->restoreState(QByteArray(b.toLatin1()));
}

PythonChartRunInput
PythonChart::createRunInput()
{
    PythonChartRunInput input;
    input.source = script->toPlainText();
    input.source.replace("$$", console->chartid);
    input.token = python->allocateRunToken();

    ScriptContext scriptContext(context);
    scriptContext.runToken = input.token;
    snapshotScriptContext(scriptContext, context, this);
    input.context = std::make_shared<const ScriptContext>(scriptContext);
    return input;
}

PythonChartOwner::PreparedRun
PythonChart::prepareRun()
{
    PythonChartOwner::PreparedRun prepared;
    if (!isVisible() || !python || !script) return prepared;

    if (script->toPlainText().isEmpty()) {
        prepared.action = PythonChartOwner::Action::Clear;
        return prepared;
    }

    prepared.action = PythonChartOwner::Action::Run;
    prepared.input = createRunInput();
    return prepared;
}

PythonRunResult
PythonChart::execScript(
    PythonChartRunInput input,
    std::shared_ptr<std::atomic_bool> cancellation)
{
    if (!python || !input.context) {
        PythonRunResult result;
        result.error = QStringLiteral("Python run context is not available.");
        result.messages << result.error;
        return result;
    }

    return python->runline(*input.context, input.source, cancellation);
}

void
PythonChart::runScript()
{
    owner.trigger();
}

void
PythonChart::applyRunResult(PythonRunResult result)
{
    QPointer<PythonChart> chart(this);
    for (const auto &command : result.chartCommands) {
        command(chart.data());
        if (!chart) return;
    }

    if (!result.messages.isEmpty()) {
        chart->console->putData(
            result.error.isEmpty()
                ? GColor(CPLOTMARKER) : QColor(Qt::red),
            result.messages.join("\n"));
    } else if (!result.error.isEmpty()) {
        chart->console->putData(
            QColor(Qt::red),
            QString("\n%1\n").arg(result.error));
    }
}

void
PythonChart::setRunBusy(bool busy)
{
    if (busy) {
        setCursor(Qt::WaitCursor);
        setUpdatesEnabled(false);
        return;
    }

    if (plot) plot->finaliseChart();
    setUpdatesEnabled(true);
    unsetCursor();
}

// rendering to a web page
void
PythonChart::webpage(QUrl url)
{
    if (canvas) canvas->setUrl(url);
}
