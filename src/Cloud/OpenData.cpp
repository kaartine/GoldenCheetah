/*
 * Copyright (c) 2018 Mark Liversedge (liversedge@gmail.com)
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

#include "OpenData.h"
#include "OpenDataCaptureUtils.h"
#include "OpenDataCaptureStateMachine.h"
#include "OpenDataExport.h"
#include "OpenDataTemporaryArchive.h"
#include "OpenDataTransport.h"
#include "OpenDataUploadWorker.h"
#include "Settings.h"
#include "Secrets.h"
#include "Colors.h"
#include "Athlete.h"
#include "RideCache.h"
#include "RideFile.h"
#include "RideItem.h"
#include "CompressedActivityFile.h"

#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QPointer>
#include <QScopedValueRollback>
#include <QThread>
#include <QTimer>

#include <memory>
#include <utility>

#ifndef OPENDATA_DEBUG
#define OPENDATA_DEBUG false
#endif
#ifdef Q_CC_MSVC
#define printd(fmt, ...) do {                                                \
    if (OPENDATA_DEBUG) {                                 \
        printf("[%s:%d %s] " fmt , __FILE__, __LINE__,        \
               __FUNCTION__, __VA_ARGS__);                    \
        fflush(stdout);                                       \
    }                                                         \
} while(0)
#else
#define printd(fmt, args...)                                            \
    do {                                                                \
        if (OPENDATA_DEBUG) {                                       \
            printf("[%s:%d %s] " fmt , __FILE__, __LINE__,              \
                   __FUNCTION__, ##args);                               \
            fflush(stdout);                                             \
        }                                                               \
    } while(0)
#endif

// Version    Date              Change
// 1          31 Mar 2018       Full OpenData Format with json summary and csv sample data

static constexpr int OpenDataVersion = 1;

namespace {

void startOpenDataUpload(
    Context *context,
    OpenDataExport::Request request,
    std::shared_ptr<OpenDataTemporaryArchive::Lease> archive);

struct OpenDataActivitySource
{
    QString allowedRoot;
    QString path;
    QString fileName;
    QString displayName;
    QString archiveName;
    OpenDataCaptureUtils::ActivityFileIdentity identity;
};

bool writeOpenDataCsv(
    const RideFile &ride,
    QIODevice *destination,
    QString &error)
{
    if (!OpenDataCaptureUtils::writeCsvHeader(
            destination, error))
        return false;

    const RideFileDataPresent *present = ride.areDataPresent();
    OpenDataCaptureUtils::ActivitySeries series;
    series.power = present->watts;
    series.heartRate = present->hr;
    series.cadence = present->cad;
    series.altitude = present->alt;
    for (const RideFilePoint *point : ride.dataPoints()) {
        if (!point) {
            error = QStringLiteral(
                "OpenData activity contains an invalid sample");
            return false;
        }
        OpenDataCaptureUtils::ActivitySample sample;
        sample.secs = point->secs;
        sample.km = point->km;
        sample.power = point->watts;
        sample.heartRate = point->hr;
        sample.cadence = point->cad;
        sample.altitude = point->alt;
        if (!OpenDataCaptureUtils::writeCsvSample(
                destination, series, sample, error)) {
            return false;
        }
    }
    return true;
}

std::unique_ptr<RideFile> openOpenDataRideFile(
    const OpenDataActivitySource &activity,
    const OpenDataTemporaryArchive::Lease &workspace,
    QStringList &errors)
{
    QString sourceError;
    std::unique_ptr<QFile> source =
        OpenDataCaptureUtils::openActivitySource(
            activity.allowedRoot,
            activity.path,
            &activity.identity,
            nullptr,
            sourceError);
    if (!source) {
        errors.append(sourceError);
        return {};
    }

    const QString snapshotTemplate =
        OpenDataCaptureUtils::activitySnapshotTemplate(
            activity.fileName);
    if (snapshotTemplate.isEmpty()) {
        errors.append(QStringLiteral(
            "OpenData activity has no file type"));
        return {};
    }
    QString temporaryError;
    const QString compression =
        QFileInfo(activity.fileName).suffix().toLower();
    const bool compressed =
        compression == QStringLiteral("zip")
        || compression == QStringLiteral("gz");
    const QString rawTemplate =
        compressed
            ? QStringLiteral("compressed-XXXXXX.%1")
                  .arg(compression)
            : snapshotTemplate;
    std::unique_ptr<
        OpenDataTemporaryArchive::TemporaryFile>
        rawSnapshot =
        workspace.createFile(
            rawTemplate,
            temporaryError);
    if (!rawSnapshot) {
        errors.append(temporaryError);
        return {};
    }

    if (!OpenDataCaptureUtils::copyActivitySnapshot(
            source.get(),
            rawSnapshot->device(),
            activity.identity.sha256,
            temporaryError)
        || !rawSnapshot->device()->flush()) {
        errors.append(
            temporaryError.isEmpty()
                ? QStringLiteral(
                      "OpenData activity snapshot is invalid")
                : temporaryError);
        return {};
    }
    source.reset();
    rawSnapshot->device()->close();

    std::unique_ptr<
        OpenDataTemporaryArchive::TemporaryFile>
        uncompressed;
    QString uncompressedPath = rawSnapshot->path();
    if (compressed) {
        uncompressed =
            workspace.createFile(
                snapshotTemplate,
                temporaryError);
        if (!uncompressed) {
            errors.append(temporaryError);
            return {};
        }
        auto compressedSource =
            std::make_unique<QFile>(
                rawSnapshot->path());
        if (!compressedSource->open(
                QIODevice::ReadOnly)) {
            errors.append(QStringLiteral(
                "Cannot read the OpenData activity snapshot"));
            return {};
        }
        const CompressedActivityFile::Format format =
            compression == QStringLiteral("zip")
                ? CompressedActivityFile::Format::Zip
                : CompressedActivityFile::Format::Gzip;
        if (!CompressedActivityFile::extractSingleFile(
                std::move(compressedSource),
                format,
                uncompressed->device())
            || !uncompressed->device()->flush()) {
            errors.append(QStringLiteral(
                "OpenData activity snapshot is invalid"));
            return {};
        }
        uncompressed->device()->close();
        uncompressedPath = uncompressed->path();
    }

    std::unique_ptr<QFile> unchanged =
        OpenDataCaptureUtils::openActivitySource(
            activity.allowedRoot,
            activity.path,
            &activity.identity,
            nullptr,
            sourceError);
    if (!unchanged) {
        errors.append(sourceError);
        return {};
    }
    unchanged.reset();

    QFile uncompressedSource(uncompressedPath);
    return std::unique_ptr<RideFile>(
        RideFileFactory::instance().openRideFile(
            nullptr, uncompressedSource, errors));
}

class OpenDataCaptureJob final : public QObject
{
public:
    explicit OpenDataCaptureJob(Context *context)
        : QObject(nullptr)
        , context_(context)
        , cyclist_(
              context && context->athlete
                  ? context->athlete->cyclist
                  : QString())
        , stateMachine_(makeOperations())
    {
        connect(
            context, &Context::athleteClose,
            this,
            [this, context](
                const QString &, Context *closingContext) {
                if (closingContext == context)
                    requestCancellation();
            });
        connect(
            context, &Context::configChanged,
            this,
            [this](qint32) {
                if (!consentGranted())
                    requestCancellation();
            });
        connect(
            context, &QObject::destroyed,
            this,
            [this]() {
                requestCancellation();
            });
        if (context
            && context->athlete
            && context->athlete->rideCache) {
            connect(
                context->athlete->rideCache,
                &RideCache::startupLoadFinished,
                this,
                [this]() {
                    scheduleAdvance();
                },
                Qt::QueuedConnection);
        }
        if (QCoreApplication *application =
                QCoreApplication::instance()) {
            connect(
                application, &QCoreApplication::aboutToQuit,
                this,
                [this]() {
                    requestCancellation();
                });
        }
    }

    ~OpenDataCaptureJob() override
    {
        archiveDescription_.reset();
        writer_.reset();
        archiveLease_.reset();
    }

    void start()
    {
        scheduleAdvance();
    }

private:
    bool consentGranted() const
    {
        return context_
            && context_->athlete
            && appsettings->cvalue(
                   cyclist_,
                   GC_OPENDATA_GRANTED,
                   QStringLiteral("X")).toString()
                == QStringLiteral("Y");
    }

    bool captureAllowed() const
    {
        return !cancellationRequested_
            && context_
            && context_->athlete
            && context_->athlete->home
            && context_->athlete->rideCache
            && consentGranted()
            && QThread::currentThread() == context_->thread();
    }

    bool startupReady() const
    {
        return captureAllowed()
            && context_->athlete->rideCache
                   ->isStartupLoadFinished();
    }

    OpenDataCaptureStateMachine::Operations makeOperations()
    {
        return {
            [this]() {
                return captureAllowed();
            },
            [this]() {
                return startupReady();
            },
            [this](qsizetype &sourceCount, QString &error) {
                return captureSnapshot(sourceCount, error);
            },
            [this](qsizetype sourceIndex, QString &error) {
                return processSource(sourceIndex, error);
            },
            [this](QString &error) {
                return validateSnapshot(error);
            },
            [this](QString &error) {
                return sealArchive(error);
            },
            [this](QString &error) {
                return describeArchive(error);
            },
            [this]() {
                handoff();
            }
        };
    }

    void requestCancellation()
    {
        cancellationRequested_ = true;
        stateMachine_.requestCancellation();
        scheduleAdvance();
    }

    void dispose(const QString &error = {})
    {
        if (completed_) return;
        completed_ = true;
        archiveDescription_.reset();
        writer_.reset();
        archiveLease_.reset();
        archivePath_.clear();
        if (!error.isEmpty())
            qWarning().noquote()
                << "Cannot capture OpenData export:" << error;
        deleteLater();
    }

    void scheduleAdvance()
    {
        if (completed_ || advanceScheduled_) return;
        advanceScheduled_ = true;
        QTimer::singleShot(0, this, [this]() {
            advanceScheduled_ = false;
            advance();
        });
    }

    void advance()
    {
        if (completed_ || processing_) return;
        QScopedValueRollback<bool> processing(processing_, true);
        QString error;
        const OpenDataCaptureStateMachine::AdvanceResult result =
            stateMachine_.advance(error);
        switch (result) {
        case OpenDataCaptureStateMachine::AdvanceResult::Waiting:
            return;
        case OpenDataCaptureStateMachine::AdvanceResult::More:
            scheduleAdvance();
            return;
        case OpenDataCaptureStateMachine::AdvanceResult::Complete:
            completed_ = true;
            deleteLater();
            return;
        case OpenDataCaptureStateMachine::AdvanceResult::Cancelled:
            dispose();
            return;
        case OpenDataCaptureStateMachine::AdvanceResult::Failed:
            dispose(error);
            return;
        }
    }

    bool captureSnapshot(
        qsizetype &sourceCount,
        QString &error)
    {
        sourceCount = 0;
        error.clear();
        if (!captureAllowed()) return true;
        request_.athleteId = context_->athlete->id.toString();
        request_.cyclist = cyclist_;
        request_.formatVersion = OpenDataVersion;

        const QString temporaryDirectory =
            context_->athlete->home->temp().absolutePath();
        archiveLease_ =
            OpenDataTemporaryArchive::Lease::create(
                temporaryDirectory, error);
        if (!archiveLease_) return false;
        archivePath_ = archiveLease_->path();
        writer_ =
            std::make_unique<OpenDataExport::ArchiveWriter>(
                archivePath_);

        std::unique_ptr<
            OpenDataTemporaryArchive::TemporaryFile>
            summaryLease;
        QString summaryPath;
        OpenDataCaptureUtils::SnapshotCaptureOperations operations;
        operations.settleRefresh =
            [this](QString &captureError) {
                return context_->athlete->rideCache
                    ->settleForOpenDataSnapshot(
                        captureError);
            };
        operations.captureManifest =
            [this, &sourceCount](QString &captureError) {
                const QVector<RideItem *> &rides =
                    context_->athlete->rideCache->rides();
                sources_.clear();
                sources_.reserve(rides.size());
                for (RideItem *item : rides) {
                    if (!item) {
                        captureError = QStringLiteral(
                            "The OpenData activity list contains "
                            "an invalid entry");
                        return false;
                    }
                    if (!OpenDataCaptureUtils::
                            includeActivityInSnapshot(
                                !item->metrics().isEmpty(),
                                item->skipsave)) {
                        continue;
                    }
                    const QString archiveName =
                        OpenDataCaptureUtils::activityArchiveName(
                            item->fileName);
                    const QString allowedRoot =
                        (item->planned
                             ? context_->athlete->home->planned()
                             : context_->athlete->home->activities())
                            .absolutePath();
                    const QString sourcePath =
                        OpenDataCaptureUtils::activitySourcePath(
                            allowedRoot,
                            item->path,
                            item->fileName);
                    OpenDataCaptureUtils::ActivityFileIdentity identity;
                    QString sourceError;
                    std::unique_ptr<QFile> source =
                        OpenDataCaptureUtils::openActivitySource(
                            allowedRoot,
                            sourcePath,
                            nullptr,
                            &identity,
                            sourceError);
                    if (archiveName.isEmpty()
                        || sourcePath.isEmpty()
                        || !source) {
                        captureError = sourceError.isEmpty()
                            ? QStringLiteral(
                                  "Invalid OpenData activity path or name")
                            : sourceError;
                        return false;
                    }
                    sources_.append({
                        allowedRoot,
                        sourcePath,
                        item->fileName,
                        item->fileName,
                        archiveName,
                        identity
                    });
                }
                request_.rideCount = sources_.size();
                sourceCount = sources_.size();
                return true;
            };
        operations.writeSummary =
            [this, &summaryLease, &summaryPath](
                QString &captureError) {
                if (!captureAllowed()) return true;
                summaryLease =
                    archiveLease_->createFile(
                        QStringLiteral("summary-XXXXXX.json"),
                        captureError);
                if (!summaryLease) return false;
                summaryPath = summaryLease->path();
                summaryLease->device()->close();
                return context_->athlete->rideCache->saveToFile(
                    true, summaryPath, captureError);
            };
        if (!OpenDataCaptureUtils::captureManifestThenSummary(
                operations, error)) {
            return false;
        }
        if (!captureAllowed()) return true;

        QFile summary(summaryPath);
        if (!summary.open(QIODevice::ReadOnly)) {
            error = QStringLiteral(
                "Cannot read the OpenData summary: %1")
                    .arg(summary.errorString());
            return false;
        }
        QJsonParseError parseError;
        const QJsonDocument summaryDocument =
            QJsonDocument::fromJson(summary.readAll(), &parseError);
        if (summary.error() != QFileDevice::NoError
            || parseError.error != QJsonParseError::NoError
            || !summaryDocument.isObject()
            || !summary.seek(0)) {
            error = QStringLiteral(
                "The OpenData summary is invalid");
            return false;
        }
        if (!writer_->addFile(
                request_.athleteId + QStringLiteral(".json"),
                &summary,
                error)) {
            return false;
        }
        return true;
    }

    bool processSource(
        qsizetype sourceIndex,
        QString &error)
    {
        error.clear();
        if (!captureAllowed()) return true;
        if (sourceIndex < 0
            || sourceIndex >= sources_.size()
            || !archiveLease_
            || !writer_) {
            error = QStringLiteral(
                "Invalid OpenData activity snapshot");
            return false;
        }

        const OpenDataActivitySource source =
            sources_.at(sourceIndex);
        QStringList errors;
        std::unique_ptr<RideFile> ride(
            openOpenDataRideFile(
                source, *archiveLease_, errors));
        if (!captureAllowed()) return true;
        if (!ride) {
            error = errors.isEmpty()
                ? QStringLiteral("Cannot read activity %1")
                      .arg(source.displayName)
                : QStringLiteral("Cannot read activity %1: %2")
                      .arg(
                          source.displayName,
                          errors.join(QStringLiteral("; ")));
            return false;
        }
        if (!ride->dataPoints().isEmpty()) {
            std::unique_ptr<
                OpenDataTemporaryArchive::TemporaryFile>
                csv =
                archiveLease_->createFile(
                    QStringLiteral(
                        "samples-XXXXXX.csv"),
                    error);
            if (!csv
                || !writeOpenDataCsv(
                    *ride, csv->device(), error)
                || !csv->device()->flush()
                || !csv->device()->seek(0)
                || !writer_->addFile(
                    source.archiveName,
                    csv->device(),
                    error)) {
                if (error.isEmpty()) {
                    error = QStringLiteral(
                        "Cannot prepare activity %1")
                                .arg(source.displayName);
                }
                return false;
            }
        }
        return true;
    }

    bool validateSnapshot(QString &error)
    {
        error.clear();
        for (const OpenDataActivitySource &source : sources_) {
            std::unique_ptr<QFile> unchanged =
                OpenDataCaptureUtils::openActivitySource(
                    source.allowedRoot,
                    source.path,
                    &source.identity,
                    nullptr,
                    error);
            if (!unchanged) return false;
        }
        return true;
    }

    bool sealArchive(QString &error)
    {
        error.clear();
        if (!writer_ || !writer_->finish(error))
            return false;
        writer_.reset();
        archiveDescription_ =
            std::make_unique<
                OpenDataExport::ArchiveDescriptionBuilder>(
                    archivePath_);
        return true;
    }

    OpenDataCaptureStateMachine::DescriptionResult
    describeArchive(QString &error)
    {
        error.clear();
        const OpenDataExport::ArchiveDescriptionResult result =
            archiveDescription_
                ? archiveDescription_->processNext(error)
                : OpenDataExport::ArchiveDescriptionResult::Invalid;
        if (result
            == OpenDataExport::ArchiveDescriptionResult::InProgress) {
            return OpenDataCaptureStateMachine::
                DescriptionResult::InProgress;
        }
        if (result
            == OpenDataExport::ArchiveDescriptionResult::Invalid) {
            return OpenDataCaptureStateMachine::
                DescriptionResult::Invalid;
        }
        return OpenDataCaptureStateMachine::
            DescriptionResult::Complete;
    }

    void handoff()
    {
        request_.archiveSize = archiveDescription_->size();
        request_.archiveSha256 = archiveDescription_->sha256();
        archiveDescription_.reset();
        request_.archivePath = archivePath_;
        const auto archiveLease = archiveLease_;
        archiveLease_.reset();
        archivePath_.clear();
        startOpenDataUpload(
            context_,
            std::move(request_),
            archiveLease);
    }

    QPointer<Context> context_;
    QString cyclist_;
    OpenDataExport::Request request_;
    QList<OpenDataActivitySource> sources_;
    QString archivePath_;
    std::shared_ptr<OpenDataTemporaryArchive::Lease> archiveLease_;
    std::unique_ptr<OpenDataExport::ArchiveWriter> writer_;
    std::unique_ptr<
        OpenDataExport::ArchiveDescriptionBuilder>
        archiveDescription_;
    OpenDataCaptureStateMachine::StateMachine stateMachine_;
    bool processing_ = false;
    bool cancellationRequested_ = false;
    bool advanceScheduled_ = false;
    bool completed_ = false;
};

void startOpenDataUpload(
    Context *context,
    OpenDataExport::Request request,
    std::shared_ptr<OpenDataTemporaryArchive::Lease> archive)
{
    QObject *owner = QCoreApplication::instance();
    if (!owner) owner = context;
    const QString cyclist = request.cyclist;
    const OpenDataTransport::Policy policy =
        OpenDataTransport::productionPolicy();
    const auto upload =
        [archive, policy](
            const OpenDataExport::Request &captured,
            const OpenDataExport::CancellationCheck &cancelled,
            const OpenDataExport::ProgressCallback &progress) {
            Q_UNUSED(archive)
            return OpenDataTransport::upload(
                captured,
                cancelled,
                progress,
                QByteArray(GC_CLOUD_OPENDATA_SECRET),
                policy);
        };

    auto *worker = new OpenDataUploadWorker(
        std::make_shared<const OpenDataExport::Request>(
            std::move(request)),
        upload,
        owner);

    QObject::connect(
        context, &QObject::destroyed,
        worker,
        [worker]() {
            worker->cancelAndJoin();
        },
        Qt::DirectConnection);
    QObject::connect(
        context, &Context::athleteClose,
        worker,
        [worker, context](const QString &, Context *closingContext) {
            if (closingContext == context)
                worker->cancelAndJoin();
        },
        Qt::DirectConnection);
    QObject::connect(
        context, &Context::configChanged,
        worker,
        [worker, cyclist](qint32) {
            if (appsettings->cvalue(
                    cyclist,
                    GC_OPENDATA_GRANTED,
                    QStringLiteral("X")).toString()
                != QStringLiteral("Y")) {
                worker->requestCancellation();
            }
        },
        Qt::DirectConnection);
    if (QCoreApplication *application =
            QCoreApplication::instance()) {
        QObject::connect(
            application, &QCoreApplication::aboutToQuit,
            worker,
            [worker]() {
                worker->cancelAndJoin();
            },
            Qt::DirectConnection);
    }
    QObject::connect(
        worker, &OpenDataUploadWorker::finished,
        owner,
        [archive]() {
            QString error;
            if (archive && !archive->remove(error))
                qWarning().noquote() << error;
        },
        Qt::DirectConnection);
    QObject::connect(
        worker, &OpenDataUploadWorker::succeeded,
        owner,
        [](const QString &cyclist, int rideCount, int formatVersion) {
            if (appsettings->cvalue(
                    cyclist,
                    GC_OPENDATA_GRANTED,
                    QStringLiteral("X")).toString()
                != QStringLiteral("Y")) {
                return;
            }
            appsettings->setCValue(
                cyclist, GC_OPENDATA_LASTPOSTED, QDate::currentDate());
            appsettings->setCValue(
                cyclist, GC_OPENDATA_LASTPOSTCOUNT, rideCount);
            appsettings->setCValue(
                cyclist, GC_OPENDATA_LASTPOSTVERSION, formatVersion);
        });
    QObject::connect(
        worker, &OpenDataUploadWorker::failed,
        owner,
        [](const QString &message) {
            qWarning().noquote() << "OpenData:" << message;
        });
    worker->startManaged();
}

} // namespace

void OpenData::check(Context *context)
{
    if (!context
        || !context->athlete
        || !context->athlete->home) {
        return;
    }

    QStringList cleanupErrors;
    OpenDataTemporaryArchive::removeAbandoned(
        context->athlete->home->temp().absolutePath(),
        cleanupErrors);
    for (const QString &cleanupError : cleanupErrors)
        qWarning().noquote() << cleanupError;

    if (!context->athlete->rideCache) return;

    const QString cyclist = context->athlete->cyclist;
    QString granted = appsettings->cvalue(
        cyclist, GC_OPENDATA_GRANTED, QStringLiteral("X")).toString();
    if (granted == QStringLiteral("N")) return;

    const int startCount = appsettings->cvalue(
        cyclist, GC_OPENDATA_RUNCOUNT, 0).toInt() + 1;
    appsettings->setCValue(
        cyclist, GC_OPENDATA_RUNCOUNT, startCount);

    if (granted == QStringLiteral("X") && startCount > 10) {
        printd("need to ask\n");
        OpenDataDialog ask(context);
        ask.exec();
    }

    granted = appsettings->cvalue(
        cyclist, GC_OPENDATA_GRANTED, QStringLiteral("X")).toString();
    const int version = appsettings->cvalue(
        cyclist, GC_OPENDATA_LASTPOSTVERSION, 0).toInt();
    const QDate lastPost = appsettings->cvalue(
        cyclist,
        GC_OPENDATA_LASTPOSTED,
        QDate(1970, 1, 1)).toDate();
    if (granted != QStringLiteral("Y")
        || (version >= OpenDataVersion
            && lastPost.daysTo(QDate::currentDate()) <= 365)) {
        return;
    }

    const int newWorkouts =
        context->athlete->rideCache->count()
        - appsettings->cvalue(
              cyclist, GC_OPENDATA_LASTPOSTCOUNT, 0).toInt();
    if (version >= OpenDataVersion && newWorkouts <= 100)
        return;

    auto *capture = new OpenDataCaptureJob(context);
    capture->start();
    printd("sending new workouts\n");
}

OpenDataDialog::OpenDataDialog(Context *context) : context(context)
{

    setWindowTitle(QString(tr("OpenData")));
    setMinimumWidth(700*dpiXFactor);
    setMinimumHeight(730*dpiYFactor);

    QVBoxLayout *layout = new QVBoxLayout(this);

    QPushButton *important = new QPushButton(style()->standardIcon(QStyle::SP_MessageBoxInformation), "", this);
    important->setFixedSize(80*dpiXFactor,80*dpiYFactor);
    important->setFlat(true);
    important->setIconSize(QSize(80*dpiXFactor,80*dpiYFactor));
    important->setAutoFillBackground(false);
    important->setFocusPolicy(Qt::NoFocus);

    QLabel *header = new QLabel(this);
    header->setWordWrap(true);
    header->setTextFormat(Qt::RichText);
    header->setText(QString(tr("<b><big>OpenData Project</big></b>")));

    QHBoxLayout *toprow = new QHBoxLayout;
    toprow->addWidget(important);
    toprow->addWidget(header);
    layout->addLayout(toprow);

    QLabel *text = new QLabel(this);
    text->setWordWrap(true);
    text->setTextFormat(Qt::RichText);
    text->setText(tr("We have started a new project to collect user activity data to enable "
                     "researchers, coaches and others to develop new models and solutions using real world data.<p>"
                     "All data that is shared is <b>anonymous</b> and cannot be traced back to the original user, no personal data is "
                     "shared and the workout data is limited to Power, Heartrate, Altitude, Cadence and Distance data along with metrics and distributions. "
                     "No personally identifiable information is collected at all.<p>"
                     "The data will be published to the general public in exactly the same format you have provided it in. And you can choose to "
                     "remove your data at any time. You can also choose to opt out again in athlete preferences.<p>"
                     "<center>Your data will only be sent once every year or so.</center>"
                     "<br>"
                     "<b>WE WILL NOT</b>:<p>"
                     "- Collect personal information <p>"
                     "- Collect GPS information <p>"
                     "- Collect notes or other metadata  <p>"
                     "<p>"
                     "<br>"
                     "<b>WE WILL</b>:<p>"
                     "- Collect basic athlete info: Gender, Year of Birth and UUID<p>"
                     "- Collect basic activity samples for HR, Cadence, Power, Distance, Altitude<p>"
                     "- Collect metrics for every activity stored for this athlete<p>"
                     "- Collect distribution and mean-max aggregates of activity data<p>"
                     "<p>"
                     "<br>"
                     "We publish the data at the <a href=\"https://cos.io/\">Center for Open Science</a> as an <a href=\"https://osf.io/6hfpz/\">OpenData project</a> open to everyone. <p>"
                     "We have also setup an <a href=\"https://github.com/GoldenCheetah/OpenData\">OpenData github project</a> to publish tools for working with the dataset.<p>"
                     ));

    scrollText = new QScrollArea();
    scrollText->setWidget(text);
    scrollText->setWidgetResizable(true);
    layout->addWidget(scrollText);

    QHBoxLayout *lastRow = new QHBoxLayout;

    proceedButton = new QPushButton(tr("Yes, I want to share"), this);
    proceedButton->setEnabled(true);
    connect(proceedButton, SIGNAL(clicked()), this, SLOT(acceptConditions()));
    abortButton = new QPushButton(tr("No thanks"), this);
    abortButton->setDefault(true);
    connect(abortButton, SIGNAL(clicked()), this, SLOT(rejectConditions()));

    lastRow->addWidget(abortButton);
    lastRow->addStretch();
    lastRow->addWidget(proceedButton);
    layout->addLayout(lastRow);

    // make YES the default if they hit return (it will be highlighted this way on screen too)
    proceedButton->setDefault(true);
    proceedButton->setAutoDefault(true);
    proceedButton->setFocus();

}

void OpenDataDialog::acceptConditions() {

    // document the decision
    appsettings->setCValue(context->athlete->cyclist, GC_OPENDATA_GRANTED, "Y");
    accept();
}

void OpenDataDialog::rejectConditions() {

    // document the decision
    appsettings->setCValue(context->athlete->cyclist, GC_OPENDATA_GRANTED, "N");
    reject();
}
