/*
 * Copyright (c) 2016 Damien Grauser (Damien.Grauser@gmail.com)
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

#include "WebPageWindow.h"

#include "MainWindow.h"
#include "RideItem.h"
#include "RideFile.h"
#include "RideImportWizard.h"
#include "IntervalItem.h"
#include "IntervalTreeView.h"
#include "SmallPlot.h"
#include "Context.h"
#include "Athlete.h"
#include "Zones.h"
#include "Settings.h"
#include "Colors.h"
#include "Units.h"
#include "TimeUtils.h"
#include "HelpWhatsThis.h"
#include "Library.h"
#include "ErgFile.h"

#include <QtWebChannel>
#include <QWebEngineProfile>
#include <QWebEngineDownloadRequest>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QPointer>
#include <QSharedPointer>
#include <QTemporaryDir>

// overlay helper
#include "AbstractView.h"
#include "GcOverlayWidget.h"
#include "IntervalSummaryWindow.h"
#include <QDebug>

#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineSettings>
#include <QWebEngineUrlSchemeHandler>
#include <QWebEngineUrlRequestJob>

// request interceptor to get downloads in QtWebEngine
class WebDownloadInterceptor : public QWebEngineUrlRequestInterceptor
{
    public:
        WebDownloadInterceptor() : QWebEngineUrlRequestInterceptor(Q_NULLPTR) {}

    public slots:
        void interceptRequest(QWebEngineUrlRequestInfo &) {
            //qDebug()<<info.requestUrl().toString();
        }
};

// custom scheme handler
class WebSchemeHandler : public QWebEngineUrlSchemeHandler
{
    public:
        WebSchemeHandler(QWebEngineView *view) : QWebEngineUrlSchemeHandler(Q_NULLPTR), view(view) {}
    public slots:
        void requestStarted(QWebEngineUrlRequestJob *request) {

            QString inject = QString("window.resolveLocalFileSystemURL = window.resolveLocalFileSystemURL || window.webkitResolveLocalFileSystemURL; "
                                     "window.resolveLocalFileSystemURL('%1', function(fs) {console.warn(fs.name);}, function(evt) {console.warn(evt.target.error.code);} );")
                             .arg(request->requestUrl().toString());
            view->page()->runJavaScript(inject);
        }

    private:
        QWebEngineView *view;
};


namespace {

QString displayOrigin(const QUrl &url)
{
    const QString scheme = url.scheme().toLower();
    if (scheme == QStringLiteral("http")
        || scheme == QStringLiteral("https")) {
        QString origin =
            scheme + QStringLiteral("://") + url.host();
        if (url.port(-1) != -1) {
            origin += QStringLiteral(":")
                + QString::number(url.port());
        }
        return origin.left(256);
    }
    return scheme + QLatin1Char(':');
}

QString cleanAbsolutePath(const QString &path)
{
    return QDir::cleanPath(
        QFileInfo(path).absoluteFilePath());
}

bool isSupportedWebImport(const QString &fileName)
{
    return ErgFile::isWorkout(fileName)
        || RideFileFactory::instance().supportedFormat(fileName);
}

} // namespace

// declared in main, we only want to use it to get QStyle
extern QApplication *application;

WebPageWindow::WebPageWindow(Context *context) : GcChartWindow(context), context(context), firstShow(true)
{
    //
    // reveal controls widget
    //

    // layout reveal controls
    QHBoxLayout *revealLayout = new QHBoxLayout;
    revealLayout->setContentsMargins(0,0,0,0);

    rButton = new QPushButton(application->style()->standardIcon(QStyle::SP_ArrowRight), "", this);
    rCustomUrl = new QLineEdit(this);
    revealLayout->addStretch();
    revealLayout->addWidget(rButton);
    revealLayout->addWidget(rCustomUrl);
    revealLayout->addStretch();

    connect(rCustomUrl, SIGNAL(returnPressed()), this, SLOT(userUrl()));
    connect(rButton, SIGNAL(clicked(bool)), this, SLOT(userUrl()));

    setRevealLayout(revealLayout);

    //
    // Chart settings
    //

    QWidget *settingsWidget = new QWidget(this);
    settingsWidget->setContentsMargins(0,0,0,0);
    HelpWhatsThis *helpSettings = new HelpWhatsThis(settingsWidget);
    settingsWidget->setWhatsThis(helpSettings->getWhatsThisText(HelpWhatsThis::Chart_Web));


    QFormLayout *commonLayout = new QFormLayout(settingsWidget);
    customUrlLabel = new QLabel(tr("URL"));
    customUrl = new QLineEdit(this);
    customUrl->setFixedWidth(250);
    customUrl->setText("");

    commonLayout->addRow(customUrlLabel, customUrl);
    commonLayout->addRow(new QLabel(tr("Hit return to apply URL")));

    setControls(settingsWidget);

    setContentsMargins(0,0,0,0);
    layout = new QVBoxLayout();
    layout->setSpacing(0);
    layout->setContentsMargins(2,0,2,2);
    setChartLayout(layout);

    view = new QWebEngineView(this);
    connect(view, SIGNAL(loadFinished(bool)), this, SLOT(loadFinished(bool)));

    // add a download interceptor
    WebDownloadInterceptor *interceptor = new WebDownloadInterceptor;
    view->page()->profile()->setUrlRequestInterceptor(interceptor);

    // add some settings
    view->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
    view->settings()->setAttribute(QWebEngineSettings::LocalStorageEnabled, true);

    view->setPage(new QWebEnginePage(context->webEngineProfile));
    view->setContentsMargins(0,0,0,0);
    view->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    view->setAcceptDrops(false);
    layout->addWidget(view);

    HelpWhatsThis *help = new HelpWhatsThis(view);
    view->setWhatsThis(help->getWhatsThisText(HelpWhatsThis::Chart_Web));

    // if we change in settings, force replot by pressing return
    connect(customUrl, SIGNAL(returnPressed()), this, SLOT(forceReplot()));

    first = true;
    configChanged(CONFIG_APPEARANCE);

    // intercept downloads
    connect(view->page()->profile(), SIGNAL(downloadRequested(QWebEngineDownloadRequest*)), this, SLOT(downloadRequested(QWebEngineDownloadRequest*)));
    connect(view->page(), SIGNAL(linkHovered(QString)), this, SLOT(linkHovered(QString)));

    forceReplot();
}

WebPageWindow::~WebPageWindow()
{
  if (view) delete view->page();
}

void 
WebPageWindow::configChanged(qint32)
{
    setProperty("color", GColor(CPLOTBACKGROUND));
    // tinted palette for headings etc
    QPalette palette;
    palette.setBrush(QPalette::Window, QBrush(GColor(CPLOTBACKGROUND)));
    palette.setColor(QPalette::WindowText, GColor(CPLOTMARKER));
    palette.setColor(QPalette::Text, GColor(CPLOTMARKER));
    palette.setColor(QPalette::Base, GCColor::alternateColor(GColor(CPLOTBACKGROUND)));
    setPalette(palette);
}

void
WebPageWindow::forceReplot()
{   
    view->setZoomFactor(dpiXFactor);
    view->setUrl(QUrl(customUrl->text()));
}

void
WebPageWindow::userUrl()
{
    // add http:// if scheme is missing
    QRegExp hasscheme("^[^:]*://.*");
    QString url = rCustomUrl->text();
    if (!hasscheme.exactMatch(url)) url = "http://" + url;
    view->setZoomFactor(dpiXFactor);
    view->setUrl(QUrl(url));
}

void
WebPageWindow::loadFinished(bool ok)
{
    QString string;
    if (ok) string = view->url().toString();
    rCustomUrl->setText(string);
}

void
WebPageWindow::finished(QNetworkReply * reply)
{
    if(reply->error() !=QNetworkReply::NoError) {
        QMessageBox::warning(0, QString("NetworkAccessManager NetworkReply"),
                           QString("QNetworkReply error string: \n") + reply->errorString(),
                           QMessageBox::Ok);
    }
}

bool
WebPageWindow::event(QEvent *event)
{
    // nasty nasty nasty hack to move widgets as soon as the widget geometry
    // is set properly by the layout system, by default the width is 100 and 
    // we wait for it to be set properly then put our helper widget on the RHS
    if (event->type() == QEvent::Resize && geometry().width() != 100) {

        // put somewhere nice on first show
        if (firstShow) {
            firstShow = false;
            //helperWidget()->move(mainWidget()->geometry().width()-275, 50);
        }

        // if off the screen move on screen
        /*if (helperWidget()->geometry().x() > geometry().width()) {
            helperWidget()->move(mainWidget()->geometry().width()-275, 50);
        }*/
    }
    return QWidget::event(event);
}

WebDownloadImportPolicy::Request
WebPageWindow::downloadRequest(QWebEngineDownloadRequest *item)
{
    WebDownloadImportPolicy::Request request;
    if (!item) {
        return request;
    }

    QWebEnginePage *sourcePage = item->page();
    request.id = item->id();
    request.sourcePage =
        reinterpret_cast<quintptr>(sourcePage);
    request.expectedPage =
        reinterpret_cast<quintptr>(
            view ? view->page() : nullptr);
    request.pageVisible = amVisible();
    request.savePageDownload =
        item->isSavePageDownload();
    request.totalBytes = item->totalBytes();
    request.pageUrl =
        sourcePage ? sourcePage->url() : QUrl();
    request.downloadUrl = item->url();
    request.suggestedFileName =
        item->suggestedFileName();
    return request;
}

bool
WebPageWindow::confirmDownload(
    const WebDownloadImportPolicy::Request &request)
{
    const QString fileName =
        WebDownloadImportPolicy::Gate::safeFileName(
            request.suggestedFileName);
    const QString text =
        tr("The web page at %1 wants to download and import "
           "%2 from %3.\n\n"
           "Continue only if you trust this file.")
            .arg(displayOrigin(request.pageUrl))
            .arg(fileName)
            .arg(displayOrigin(request.downloadUrl));

    QMessageBox prompt(
        QMessageBox::Question,
        tr("Download and Import"),
        text,
        QMessageBox::Yes | QMessageBox::No,
        this);
    prompt.setTextFormat(Qt::PlainText);
    prompt.setDefaultButton(QMessageBox::No);
    prompt.setEscapeButton(QMessageBox::No);
    return prompt.exec() == QMessageBox::Yes;
}

void
WebPageWindow::downloadRequested(
    QWebEngineDownloadRequest *item)
{
    if (!item) {
        return;
    }

    const WebDownloadImportPolicy::Request request =
        downloadRequest(item);
    const WebDownloadImportPolicy::Decision preflight =
        downloadImportGate.handleRequest(
            request,
            QString(),
            WebDownloadImportPolicy::UserDecision::NotAsked);

    if (preflight.action
        == WebDownloadImportPolicy::RequestAction::Ignore) {
        return;
    }
    if (preflight.action
        != WebDownloadImportPolicy::RequestAction::Confirm
        || !isSupportedWebImport(
            WebDownloadImportPolicy::Gate::safeFileName(
                request.suggestedFileName))) {
        downloadImportGate.cancel(request.id);
        item->cancel();
        return;
    }

    QPointer<WebPageWindow> window(this);
    QPointer<QWebEngineDownloadRequest> download(item);
    const bool approved = confirmDownload(request);
    if (!window) {
        if (download) {
            download->cancel();
        }
        return;
    }
    if (!download) {
        downloadImportGate.cancel(request.id);
        return;
    }
    if (!approved) {
        downloadImportGate.handleRequest(
            request,
            QString(),
            WebDownloadImportPolicy::UserDecision::Rejected);
        item->cancel();
        return;
    }

    if (item->state()
            != QWebEngineDownloadRequest::DownloadRequested
        || !Context::isValid(context)
        || !context->athlete
        || !context->athlete->home) {
        downloadImportGate.cancel(request.id);
        item->cancel();
        return;
    }

    const QString stagingTemplate =
        context->athlete->home->temp().absoluteFilePath(
            QStringLiteral("web-download-XXXXXX"));
    const QSharedPointer<QTemporaryDir> staging =
        QSharedPointer<QTemporaryDir>::create(stagingTemplate);
    if (!staging->isValid()) {
        downloadImportGate.cancel(request.id);
        item->cancel();
        QMessageBox::warning(
            this,
            tr("Download Failed"),
            tr("A private temporary directory could not be created."));
        return;
    }

    const WebDownloadImportPolicy::Request currentRequest =
        downloadRequest(item);
    const WebDownloadImportPolicy::Decision decision =
        downloadImportGate.handleRequest(
            currentRequest,
            staging->path(),
            WebDownloadImportPolicy::UserDecision::Approved);
    if (decision.action
        != WebDownloadImportPolicy::RequestAction::Accept) {
        item->cancel();
        return;
    }

    item->setDownloadDirectory(staging->path());
    item->setDownloadFileName(decision.fileName);
    const QString configuredPath = cleanAbsolutePath(
        QDir(item->downloadDirectory()).absoluteFilePath(
            item->downloadFileName()));
    if (configuredPath != decision.filePath) {
        downloadImportGate.cancel(request.id);
        item->cancel();
        return;
    }

    downloadStaging.insert(request.id, staging);
    connect(
        item,
        &QWebEngineDownloadRequest::isFinishedChanged,
        this,
        &WebPageWindow::downloadFinished,
        Qt::UniqueConnection);
    connect(
        item,
        &QObject::destroyed,
        this,
        [this, id = request.id]() {
            downloadImportGate.cancel(id);
            downloadStaging.remove(id);
        });

    item->accept();
}

void
WebPageWindow::downloadFinished()
{
    QWebEngineDownloadRequest *item =
        qobject_cast<QWebEngineDownloadRequest *>(
            sender());
    if (!item || !item->isFinished()) {
        return;
    }

    const quint32 id = item->id();
    const QSharedPointer<QTemporaryDir> staging =
        downloadStaging.take(id);
    WebDownloadImportPolicy::Completion completion;
    completion.id = id;
    completion.sourcePage =
        reinterpret_cast<quintptr>(item->page());
    completion.filePath = cleanAbsolutePath(
        QDir(item->downloadDirectory()).absoluteFilePath(
            item->downloadFileName()));
    completion.finished = item->isFinished();
    completion.succeeded =
        item->state()
        == QWebEngineDownloadRequest::DownloadCompleted;

    const QString fileName =
        downloadImportGate.takeCompletedImport(completion);
    if (fileName.isEmpty() || !staging) {
        if (completion.succeeded) {
            QMessageBox::warning(
                this,
                tr("Download Failed"),
                tr("The downloaded file could not be verified "
                   "and was not imported."));
        }
        return;
    }
    if (!isSupportedWebImport(fileName)) {
        QMessageBox::warning(
            this,
            tr("Unsupported Download"),
            tr("The downloaded file type is not supported "
               "for activity or workout import."));
        return;
    }

    if (ErgFile::isWorkout(fileName)) {
        Library::importFiles(
            context,
            QStringList{fileName},
            LibraryBatchImportConfirmation::forcedDialog);
        return;
    }

    RideImportWizard *dialog =
        new RideImportWizard(
            QStringList{fileName}, context);
    QObject::connect(
        dialog,
        &QObject::destroyed,
        dialog,
        [staging]() {
            Q_UNUSED(staging);
        });
    dialog->process();
}

void
WebPageWindow::downloadProgress(qint64 a, qint64 b)
{
    Q_UNUSED(a)
    Q_UNUSED(b)
    //qDebug()<<"downloading..." << a<< b;
}

void
WebPageWindow::linkHovered(QString)
{
    //qDebug()<<"hovering over:" << link;
}
