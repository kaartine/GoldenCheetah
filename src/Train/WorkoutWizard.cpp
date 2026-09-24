/*
 * Copyright (c) 2010 Greg Lonnon (greg.lonnon@gmail.com)
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

#include "WorkoutWizard.h"
#include "MainWindow.h"
#include "Context.h"
#include "Athlete.h"
#include "Colors.h"
#include "HelpWhatsThis.h"

#include "Library.h"
#include "LibraryParser.h"
#include "TrainDB.h"
#include "WorkoutFileWriter.h"
#include "WorkoutGenerator.h"

#include "qwt_plot.h"
#include "qwt_plot_curve.h"
#include <QCheckBox>
#include <QComboBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <limits>
#include <memory>

/// workout plot
class WorkoutPlot: public QwtPlot
{
    QwtPlotCurve *workoutCurve;
public:
    WorkoutPlot()
    {
        workoutCurve = new QwtPlotCurve();
        setTitle("Workout Chart");
        QPen pen = QPen(Qt::blue,1.0);
        workoutCurve->setPen(pen);
        QColor brush_color = QColor(124, 91, 31);
        brush_color.setAlpha(64);
        workoutCurve->setBrush(brush_color);
        workoutCurve->attach(this);
    }
    void setYAxisTitle(QString title)
    {
        setAxisTitle(QwtAxis::YLeft, title);
    }
    void setXAxisTitle(QString title)
    {
        setAxisTitle(QwtAxis::XBottom,title);
    }
    void setData(QVector<double> &xData, QVector<double> &yData)
    {
        workoutCurve->setSamples(xData, yData);
    }
};

//// Workout Editor

WorkoutEditorBase::WorkoutEditorBase(QStringList &colms, QWidget *parent) :QFrame(parent)
{
    QVBoxLayout *layout = new QVBoxLayout();
    QHBoxLayout *row1Layout = new QHBoxLayout();
    table = new QTableWidget();

    table->setColumnCount(colms.count());
    table->setHorizontalHeaderLabels(colms);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->setShowGrid(true);
    table->setAlternatingRowColors(true);
    table->resizeColumnsToContents();
    row1Layout->addWidget(table);

    connect(table,SIGNAL(cellChanged(int,int)),this,SLOT(cellChanged(int,int)));


    QHBoxLayout *row2Layout = new QHBoxLayout();
    QPushButton *delButton = new QPushButton();
    delButton->setText(tr("Delete"));
    delButton->setToolTip(tr("Delete the highlighted row"));
    connect(delButton,SIGNAL(clicked()),this,SLOT(delButtonClicked()));
    row2Layout->addWidget(delButton);
    QPushButton *addButton = new QPushButton();
    addButton->setText(tr("Add"));
    addButton->setToolTip(tr("Add row at end"));
    connect(addButton,SIGNAL(clicked()),this,SLOT(addButtonClicked()));
    row2Layout->addWidget(addButton);
    QPushButton *insertButton = new QPushButton();
    insertButton->setText(tr("Insert"));
    insertButton->setToolTip(tr("Add a Row above the highlighted row"));
    connect(insertButton,SIGNAL(clicked()),this,SLOT(insertButtonClicked()));
    row2Layout->addWidget(insertButton);
    QPushButton *lapButton = new QPushButton();
    lapButton->setText(tr("Lap"));
    lapButton->setToolTip(tr("Add a Lap below the highlighted row"));
    row2Layout->addWidget(lapButton);
    connect(lapButton,SIGNAL(clicked()),this,SLOT(lapButtonClicked()));
    layout->addLayout(row1Layout);
    layout->addLayout(row2Layout);
    setLayout(layout);
}



void WorkoutEditorAbs::insertDataRow(int row)
{
    table->insertRow(row);
    // minutes colm can be doubles
    table->setItem(row,0,new WorkoutItemDouble());
    // wattage must be integer
    table->setItem(row,1,new WorkoutItemInt());
}

void WorkoutEditorRel::insertDataRow(int row)
{
    table->insertRow(row);
    // minutes colm can be doubles
    table->setItem(row,0,new WorkoutItemDouble());
    // precentage of ftp
    table->setItem(row,1,new WorkoutItemDouble());
    // current ftp
    WorkoutItemInt *watts = new WorkoutItemInt();
    watts->setFlags(watts->flags() & (~Qt::ItemIsEditable));
    table->setItem(row,2,watts);
}

template<int minGrade, int maxGrade>
class WorkoutItemDoubleRange : public WorkoutItemDouble
{
    int min, max;
public:
    WorkoutItemDoubleRange() : min(minGrade), max(maxGrade) {}
    QString validateData(const QString &text)
    {
        double d = text.toDouble();
        d = d > max ? max : d;
        d = d < min ? min : d;
        return QString::number(d);
    }

};

void WorkoutEditorGradient::insertDataRow(int row)
{
    table->insertRow(row);
    // distance
    table->setItem(row,0,new WorkoutItemDouble());
    // grade
    table->setItem(row,1,new WorkoutItemDoubleRange<-40,40>());
}

///  Workout Summary

void WorkoutMetricsSummary::updateMetrics(QStringList &order, QHash<QString,RideMetricPtr>  &metrics)
{
    foreach(QString name, order)
    {
        RideMetricPtr rmp = metrics[name];
        if(!metricMap.contains(name))
        {
            QLabel *label = new QLabel((rmp->name()) + ":");
			label->setTextFormat(Qt::RichText);
            QLabel *lcd = new QLabel();
            metricMap[name] = QPair<QLabel*,QLabel*>(label,lcd);
            layout->addWidget(label,metricMap.size(),0);
            layout->addWidget(lcd,metricMap.size(),1);
        }
        QLabel *lcd = metricMap[name].second;
        if(name == "time_riding")
        {
            QTime start (0,0,0);
            QTime time = start.addSecs(rmp->value(true));
            QString s = time.toString("HH:mm:ss");
            //qDebug() << s << " " << time.second();
            lcd->setText(s);
        }
        else
        {
            lcd->setText(QString::number(rmp->value(true),'f',rmp->precision()) + " " + (rmp->units(true)) );
        }
        //qDebug() << name << ":" << (int)rmp->value(true);
    }
}

void WorkoutMetricsSummary::updateMetrics(QMap<QString, QString> &map)
{
    QMap<QString, QString>::iterator i = map.begin();
    while(i != map.end())
    {
        if(!metricMap.contains(i.key()))
        {
            QLabel *label = new QLabel((i.key() + ":"));
            QLabel *value = new QLabel();
            metricMap[i.key()] = QPair<QLabel*,QLabel*>(label,value);
            layout->addWidget(label,metricMap.size(),0);
            layout->addWidget(value,metricMap.size(),1);
        }
        QLabel *value = metricMap[i.key()].second;
        value->setText(i.value());
        ++i;
    }
}

/// WorkoutTypePage

WorkoutTypePage::WorkoutTypePage(Context *context, QWidget *parent)
    : QWizardPage(parent), context(context)
{
}

void WorkoutTypePage::initializePage()
{
    if (buttonGroupBox) return;
    setTitle(tr("Workout Creator"));
    setSubTitle(tr("Select the workout type to be created"));
    buttonGroupBox = new QButtonGroup(this);
    generatedRadioButton = new QRadioButton(tr("Generate for a training goal"));
    generatedRadioButton->setChecked(true);
    absWattageRadioButton = new QRadioButton(tr("Absolute Wattage"));
    relWattageRadioButton = new QRadioButton(tr("Relative Wattage"));
    gradientRadioButton = new QRadioButton(tr("Gradient"));

    if (context && context->rideItem()) {
        QString s = context->rideItem()->ride()->startTime().toLocalTime().toString();
        QString importStr = tr("Import Selected Activity (") + s + ")";
        importRadioButton = new QRadioButton((importStr));
    } else {
        importRadioButton = new QRadioButton(tr("No activity selected"));
        importRadioButton->setEnabled(false);
    }
    QVBoxLayout *groupBoxLayout = new QVBoxLayout();

    groupBoxLayout->addWidget(generatedRadioButton);
    groupBoxLayout->addWidget(absWattageRadioButton);
    groupBoxLayout->addWidget(relWattageRadioButton);
    groupBoxLayout->addWidget(gradientRadioButton);
    groupBoxLayout->addWidget(importRadioButton);
    registerField("generatedWorkout",generatedRadioButton);
    registerField("absWattage",absWattageRadioButton);
    registerField("relWattage",relWattageRadioButton);
    registerField("gradientWattage",gradientRadioButton);
    registerField("import",importRadioButton);
    setLayout(groupBoxLayout);
}

int WorkoutTypePage::nextId() const
{
    if(generatedRadioButton->isChecked())
        return WorkoutWizard::WW_GeneratedWorkoutPage;
    else if(absWattageRadioButton->isChecked())
        return WorkoutWizard::WW_AbsWattagePage;
    else if (relWattageRadioButton->isChecked())
        return WorkoutWizard::WW_RelWattagePage;
    else if (gradientRadioButton->isChecked())
        return WorkoutWizard::WW_GradientPage;
    else if (importRadioButton->isChecked())
        return WorkoutWizard::WW_ImportPage;
    return WorkoutWizard::WW_AbsWattagePage;
}


//// AbsWattagePage

AbsWattagePage::AbsWattagePage(Context *context, QWidget *parent)
    : WorkoutPage(context, parent) {}

void AbsWattagePage::initializePage()
{
    if (we) return;
    setTitle(tr("Workout Wizard"));
    setSubTitle(tr("Absolute Wattage Workout Creator"));
    QHBoxLayout *layout = new QHBoxLayout();
    setLayout(layout);
    QStringList colms;
    colms.append(tr("Minutes"));
    colms.append(tr("Wattage"));
    we = new WorkoutEditorAbs(colms);
    layout->addWidget(we);
    QVBoxLayout *summaryLayout = new QVBoxLayout();
    metricsSummary = new WorkoutMetricsSummary();
    summaryLayout->addWidget(metricsSummary);
    plot = new WorkoutPlot();
    plot->setYAxisTitle(tr("Wattage"));
    plot->setXAxisTitle(tr("Time (minutes)"));
    plot->setAxisScale(QwtAxis::YLeft,0,500,0);
    plot->setAxisScale(QwtAxis::XBottom,0,120,0);
    summaryLayout->addWidget(plot);
    summaryLayout->addStretch(1);
    layout->addLayout(summaryLayout);
    layout->addStretch(1);
    connect(we,SIGNAL(dataChanged()),this,SLOT(updateMetrics()));
    updateMetrics();
}

void AbsWattagePage::updateMetrics()
{
    QVector<QPair<QString,QString> > data;
    QVector<double> x;
    QVector<double> y;

    we->rawData(data);

    int curSecs = 0;
    // create rideFile
    QSharedPointer<RideFile> workout(new RideFile());
    workout->context = context;
    workout->setRecIntSecs(1);
    double curMin = 0;
    for(int i = 0; i < data.size() ; i++)
    {
        if(data[i].first == "LAP") continue;

        double min = data[i].first.toDouble();
        double watts = data[i].second.toDouble();
        int secs = min * 60;
        for(int j = 0; j < secs; j++)
        {
            RideFilePoint rfp;
            rfp.secs = curSecs++;
            rfp.watts = watts;
            rfp.cad = 90; // to be able to calculate the metrics - since time_riding depens on cad and kph
            rfp.kph = 25; // to be able to calculate the metrics - since time_riding depens on cad and kph
            workout->appendPoint(rfp);
        }

        x.append(curMin);
        y.append(watts);
        curMin += min;
        x.append(curMin);
        y.append(watts);

    }
    // replot workoutplot
    plot->setAxisAutoScale(QwtAxis::YLeft);
    plot->setAxisAutoScale(QwtAxis::XBottom);
    plot->setData(x,y);
    plot->replot();

    // calculate bike score, xpower
    QStringList metrics;
    metrics.append("time_riding");
    metrics.append("total_work");
    metrics.append("average_power");
    metrics.append("skiba_bike_score");
    metrics.append("skiba_xpower");

#if 0 //XXX REFACTOR METRICS
    const RideMetricFactory &factory = RideMetricFactory::instance();
    const RideMetric *rm = factory.rideMetric("skiba_xpower");
    QHash<QString,RideMetricPtr> results = rm->computeMetrics(NULL,&*workout,context->athlete->zones("Bike"),context->athlete->hrZones(),metrics);
    metricsSummary->updateMetrics(metrics,results);
#endif
}

bool AbsWattagePage::SaveWorkout()
{
    QString workoutDir = appsettings->value(this,GC_WORKOUTDIR).toString();

    QString filename = QFileDialog::getSaveFileName(this,QString(tr("Save Workout")),
                                                    workoutDir,tr("Computrainer Format *.erg"));
    if (filename.isEmpty()) return false;

    filename = WorkoutFileWriter::ensureSuffix(filename, QStringLiteral(".erg"));
    WorkoutFileWriter writer(filename);
    QString writeError;
    if (!writer.open(writeError)) {
        QMessageBox::critical(this, tr("Save Workout"),
                tr("Unable to save %1:\n%2").arg(filename, writeError));
        return false;
    }
    QTextStream &stream = writer.stream();
    // create the header
    SaveWorkoutHeader(stream, filename, QString("golden cheetah"), QString("MINUTES WATTS"));
    QVector<QPair<QString, QString> > rawData;
    we->rawData(rawData);
    double currentX = 0;
    stream << "[COURSE DATA]" << Qt::endl;
    QPair<QString, QString > p;
    foreach (p,rawData)
    {
        if(p.first == "LAP")
        {
            stream << currentX << " LAP" << Qt::endl;
        }
        else
        {
            stream << currentX << " " << p.second << Qt::endl;
            currentX += p.first.toDouble();
            stream << currentX << " " << p.second << Qt::endl;
        }
    }
    stream << "[END COURSE DATA]" << Qt::endl;
    if (!writer.commit(writeError)) {
        QMessageBox::critical(this, tr("Save Workout"),
                tr("Unable to save %1:\n%2").arg(filename, writeError));
        return false;
    }

    // import them via the workoutimporter
    QStringList files;
    files << filename;
    const LibraryImportResult imported = Library::importFiles(
            context, files, LibraryBatchImportConfirmation::noDialog);
    if (!imported.allSucceeded()) {
        QMessageBox::warning(this, tr("Save Workout"),
                tr("The workout was saved but could not be added to the workout library."));
        return false;
    }
    return true;
}

/// RelativeWattagePage

RelWattagePage::RelWattagePage(Context *context, QWidget *parent)
    : WorkoutPage(context, parent) {}

void RelWattagePage::initializePage()
{
    if (we) return;
    if (context && context->athlete && context->athlete->zones("Bike") && context->athlete->zones("Bike")->whichRange(QDate::currentDate()) >= 0) {
        int zoneRange = context->athlete->zones("Bike")->whichRange(QDate::currentDate());
        ftp = context->athlete->zones("Bike")->getCP(zoneRange);
    } else {
        ftp = 100; // if zones are not available let's make absolute watts match percentajes
    }

    setTitle(tr("Workout Wizard"));
    QString subTitle = tr("Relative Wattage Workout Creator, current CP = ") + QString::number(ftp);
    setSubTitle(subTitle);

    plot = new WorkoutPlot();
    plot->setYAxisTitle(tr("%"));
    plot->setXAxisTitle(tr("Time (minutes)"));
    plot->setAxisScale(QwtAxis::YLeft,0,200,0);
    plot->setAxisScale(QwtAxis::XBottom,0,120,0);

    QHBoxLayout *layout = new QHBoxLayout();
    setLayout(layout);
    QStringList colms;
    colms.append(tr("Minutes"));
    colms.append(tr("%"));
    colms.append(tr("Wattage"));
    we = new WorkoutEditorRel(colms,ftp);
    layout->addWidget(we);
    QVBoxLayout *summaryLayout = new QVBoxLayout();
    metricsSummary = new WorkoutMetricsSummary();
    summaryLayout->addWidget(metricsSummary);
    summaryLayout->addWidget(plot,1);
    layout->addLayout(summaryLayout);
    layout->addStretch(1);
    connect(we,SIGNAL(dataChanged()),this,SLOT(updateMetrics()));
    updateMetrics();
}

void RelWattagePage::updateMetrics()
{
    QVector<QPair<QString,QString> > data;
    QVector<double> x;
    QVector<double> y;

    we->rawData(data);

    int curSecs = 0;
    // create rideFile
    QSharedPointer<RideFile> workout(new RideFile());
    workout->context = context;
    workout->setRecIntSecs(1);
    for(int i = 0; i < data.size() ; i++)
    {
        if(data[i].first == "LAP") continue;
        double min = data[i].first.toDouble();
        double percentFtp = data[i].second.toDouble();
        int secs = min * 60;
        x.append(curSecs/60);
        y.append(percentFtp);
        for(int j = 0; j < secs; j++)
        {
            RideFilePoint rfp;
            rfp.secs = curSecs++;
            rfp.watts = percentFtp * ftp /100;
            rfp.cad = 90; // to be able to calculate the metrics - since time_riding depens on cad and kph
            rfp.kph = 25; // to be able to calculate the metrics - since time_riding depens on cad and kph
            workout->appendPoint(rfp);
        }
        x.append(curSecs/60);
        y.append(percentFtp);
    }

    // replot workoutplot
    plot->setAxisAutoScale(QwtAxis::YLeft);
    plot->setAxisAutoScale(QwtAxis::XBottom);
    plot->setData(x,y);;
    plot->replot();

    // calculate bike score, xpower
    QStringList metrics;
    metrics.append("time_riding");
    metrics.append("total_work");
    metrics.append("average_power");
    metrics.append("skiba_bike_score");
    metrics.append("skiba_xpower");
#if 0 //XXX REFACTOR METRICS
    const RideMetricFactory &factory = RideMetricFactory::instance();
    const RideMetric *rm = factory.rideMetric("skiba_xpower");
    QHash<QString,RideMetricPtr> results = rm->computeMetrics(NULL,&*workout,context->athlete->zones("Bike"),context->athlete->hrZones(),metrics);
    metricsSummary->updateMetrics(metrics,results);
#endif
}

bool RelWattagePage::SaveWorkout()
{
    QString workoutDir = appsettings->value(this,GC_WORKOUTDIR).toString();

    QString filename = QFileDialog::getSaveFileName(this,QString(tr("Save Workout")),
                                                    workoutDir,tr("Computrainer Format *.mrc"));
    if (filename.isEmpty()) return false;

    filename = WorkoutFileWriter::ensureSuffix(filename, QStringLiteral(".mrc"));
    WorkoutFileWriter writer(filename);
    QString writeError;
    if (!writer.open(writeError)) {
        QMessageBox::critical(this, tr("Save Workout"),
                tr("Unable to save %1:\n%2").arg(filename, writeError));
        return false;
    }
    QTextStream &stream = writer.stream();
    // create the header
    SaveWorkoutHeader(stream, filename, QString("golden cheetah"), QString("MINUTES PERCENT"));
    QVector<QPair<QString, QString> > rawData;
    we->rawData(rawData);
    double currentX = 0;
    stream << "[COURSE DATA]" << Qt::endl;
    QPair<QString, QString > p;
    foreach (p,rawData)
    {
        if(p.first == "LAP")
        {
            stream << currentX << " LAP" << Qt::endl;
        }
        else
        {
            stream << currentX << " " << p.second << Qt::endl;
            currentX += p.first.toDouble();
            stream << currentX << " " << p.second << Qt::endl;
        }
    }
    stream << "[END COURSE DATA]" << Qt::endl;
    if (!writer.commit(writeError)) {
        QMessageBox::critical(this, tr("Save Workout"),
                tr("Unable to save %1:\n%2").arg(filename, writeError));
        return false;
    }

    // import them via the workoutimporter
    QStringList files;
    files << filename;
    const LibraryImportResult imported = Library::importFiles(
            context, files, LibraryBatchImportConfirmation::noDialog);
    if (!imported.allSucceeded()) {
        QMessageBox::warning(this, tr("Save Workout"),
                tr("The workout was saved but could not be added to the workout library."));
        return false;
    }
    return true;
}

/// GradientPage
GradientPage::GradientPage(Context *context, QWidget *parent)
    : WorkoutPage(context, parent) {}

void GradientPage::initializePage()
{
    if (we) return;
    metricUnits = GlobalContext::context()->useMetricUnits;
    setTitle(tr("Workout Wizard"));

    setSubTitle(tr("Manually create a workout based on gradient (slope) and distance, maximum grade is +-40."));

    QHBoxLayout *layout = new QHBoxLayout();
    setLayout(layout);
    QStringList colms;

    colms.append(metricUnits ? tr("KM") : tr("Miles"));
    colms.append(tr("Grade"));
    we = new WorkoutEditorGradient(colms);
    layout->addWidget(we);
    QVBoxLayout *summaryLayout = new QVBoxLayout();
    metricsSummary = new WorkoutMetricsSummary();
    summaryLayout->addWidget(metricsSummary);
    summaryLayout->addStretch(1);
    layout->addLayout(summaryLayout);
    layout->addStretch(1);
    connect(we,SIGNAL(dataChanged()),this,SLOT(updateMetrics()));
    updateMetrics();
}

void GradientPage::updateMetrics()
{
    QVector<QPair<QString,QString> > data;
    we->rawData(data);

    double totalDistance = 0.0;
    double gain = 0;

    // create rideFile
    QSharedPointer<RideFile> workout(new RideFile());
    workout->setRecIntSecs(1);
    for(int i = 0; i < data.size() ; i++)
    {
        if(data[i].first == "LAP") continue;
        double distance = data[i].first.toDouble();
        double grade = data[i].second.toDouble();
        constexpr double FeetPerMile = 5280.0;
        double delta = distance * (metricUnits ? 1000.0 : FeetPerMile)
                * grade / 100.0;
        gain += (delta > 0) ? delta : 0;
        totalDistance += distance;
    }
    QMap<QString,QString> metricSummaryMap;
    QString s = (metricUnits ? tr("KM") : tr("Miles"));
    metricSummaryMap[s] = QString::number(totalDistance);
    s = (metricUnits ? tr("Meters Gained") : tr("Feet Gained"));
    metricSummaryMap[s] = QString::number(gain);
    metricsSummary->updateMetrics(metricSummaryMap);
}

bool GradientPage::SaveWorkout()
{
    QString workoutDir = appsettings->value(this,GC_WORKOUTDIR).toString();

    QString filename = QFileDialog::getSaveFileName(this,QString(tr("Save Workout")),
                                                    workoutDir,tr("Computrainer Format *.crs"));
    if (filename.isEmpty()) return false;

    filename = WorkoutFileWriter::ensureSuffix(filename, QStringLiteral(".crs"));
    WorkoutFileWriter writer(filename);
    QString writeError;
    if (!writer.open(writeError)) {
        QMessageBox::critical(this, tr("Save Workout"),
                tr("Unable to save %1:\n%2").arg(filename, writeError));
        return false;
    }
    QTextStream &stream = writer.stream();
    // create the header
    SaveWorkoutHeader(stream, filename, QString("golden cheetah"), QString("DISTANCE GRADE WIND"));
    QVector<QPair<QString, QString> > rawData;
    we->rawData(rawData);
    stream << "[COURSE DATA]" << Qt::endl;
    QPair<QString, QString > p;
    foreach (p,rawData)
    {
        if(p.first == "LAP")
        {
            stream << "LAP" << Qt::endl;
        }
        else
        {
            // header indicates metric units, so convert accordingly
            double currentX = p.first.toDouble()*(metricUnits ? 1.0 : KM_PER_MILE);
            stream << currentX << " " << p.second << " 0" << Qt::endl;
        }
    }
    stream << "[END COURSE DATA]" << Qt::endl;
    if (!writer.commit(writeError)) {
        QMessageBox::critical(this, tr("Save Workout"),
                tr("Unable to save %1:\n%2").arg(filename, writeError));
        return false;
    }

    // import them via the workoutimporter
    QStringList files;
    files << filename;
    const LibraryImportResult imported = Library::importFiles(
            context, files, LibraryBatchImportConfirmation::noDialog);
    if (!imported.allSucceeded()) {
        QMessageBox::warning(this, tr("Save Workout"),
                tr("The workout was saved but could not be added to the workout library."));
        return false;
    }
    return true;
}



ImportPage::ImportPage(Context *context, QWidget *parent)
    : WorkoutPage(context, parent) {}

void ImportPage::initializePage()
{
        if (plot) return;
        RideItem *rideItem = context ? context->rideItem() : nullptr;
        if (NULL == rideItem || rideItem->ride()->dataPoints().isEmpty()) {
            setTitle(tr("Workout Wizard"));
            setSubTitle(tr("The selected activity has no samples to import."));
            setFinalPage(false);
            return;
        }
        validInput = true;

        setTitle(tr("Workout Wizard"));
        setSubTitle(tr("Import current activity as a Gradient ride (slope based)"));
        setFinalPage(true);
        plot = new WorkoutPlot();
        metricUnits = GlobalContext::context()->useMetricUnits;
        QString s = (metricUnits ? tr("KM") : tr("Miles"));
        QString distance = QString(tr("Distance (")) + s + QString(")");
        plot->setXAxisTitle(distance);
        s = (metricUnits ? tr("Meters") : tr("Feet"));
        QString elevation = QString(tr("elevation (")) + s + QString(")");
        plot->setYAxisTitle(elevation);


        foreach(RideFilePoint *rfp,rideItem->ride()->dataPoints())
        {
            rideData.append(QPair<double,double>(rfp->km,rfp->alt));
        }
        QVBoxLayout *layout = new QVBoxLayout();
        layout->addWidget(plot,1);

        QGroupBox *spinGroupBox = new QGroupBox();
        spinGroupBox->setTitle(tr("Smoothing Parameters"));
        QLabel *gradeLabel = new QLabel(tr("Maximum Grade"));
        gradeBox = new QSpinBox();
        gradeBox->setValue(20);
        gradeBox->setMaximum(40);
        gradeBox->setMinimum(1);
        gradeBox->setToolTip(tr("Maximum supported grade is +-40"));
        connect(gradeBox,SIGNAL(valueChanged(int)),this,SLOT(updatePlot()));

        segmentBox = new QSpinBox();
        segmentBox->setMinimum(0);
        constexpr int FeetPerMile = 5280;
        segmentBox->setMaximum((metricUnits ? 1000 : FeetPerMile));
        segmentBox->setValue((metricUnits ? 1000 : FeetPerMile)/2);
        segmentBox->setSingleStep((metricUnits ? 1000 : FeetPerMile)/100);
        s = QString(tr("Segment length is based on"))+ " " + QString(metricUnits ? tr("meters"): tr("feet"));
        segmentBox->setToolTip((s));
        connect(segmentBox,SIGNAL(valueChanged(int)),this,SLOT(updatePlot()));
        QHBoxLayout *bottomLayout = new QHBoxLayout();
        QGridLayout *spinBoxLayout = new QGridLayout();
        spinBoxLayout->addWidget(gradeLabel,0,0);
        spinBoxLayout->addWidget(gradeBox,0,1);
        spinBoxLayout->addWidget(new QLabel(tr("Segment Length")),1,0);
        spinBoxLayout->addWidget(segmentBox,1,1);
        spinGroupBox->setLayout(spinBoxLayout);
        bottomLayout->addWidget(spinGroupBox);
        metricsSummary = new WorkoutMetricsSummary();
        bottomLayout->addWidget(metricsSummary);

        layout->addLayout(bottomLayout);
        setLayout(layout);
        updatePlot();
}

void ImportPage::updatePlot()
{
    if (rideData.isEmpty() || !gradeBox || !segmentBox) return;
    QVector<double> x;
    QVector<double> y;
    QPair<double,double> p;

    int segmentLength = segmentBox->value();
    double maxSlope = gradeBox->value();

    double curAlt = rideData.at(0).second
            * (!metricUnits ? FEET_PER_METER : 1);
    rideProfile.clear();
    double startDistance = 0;
    double curDistance = 0;
    double startAlt = rideData.at(0).second * (!metricUnits ? FEET_PER_METER : 1);
    double totalDistance = 0;
    foreach(p, rideData)
    {
        totalDistance = p.first * (!metricUnits ? MILES_PER_KM : 1);
        curAlt = p.second * (!metricUnits ? FEET_PER_METER : 1);
        constexpr double FeetPerMile = 5280.0;
        curDistance = (totalDistance - startDistance)
                * (metricUnits ? 1000.0 : FeetPerMile);
        if(curDistance > segmentLength)
        {
            double slope = (curAlt - startAlt) / curDistance * 100;
            slope = std::clamp(slope, -maxSlope, maxSlope);
            double alt = startAlt + (slope * curDistance / 100);
            x.append(totalDistance);
            y.append(alt);
            rideProfile.append(QPair<double,double>(totalDistance,slope));
            startDistance = totalDistance;
            startAlt = curAlt;
        }
    }
    if (totalDistance > startDistance && curDistance > 0.0) {
        double slope = (curAlt - startAlt) / curDistance * 100.0;
        slope = std::clamp(slope, -maxSlope, maxSlope);
        const double alt = startAlt + slope * curDistance / 100.0;
        x.append(totalDistance);
        y.append(alt);
        rideProfile.append(QPair<double,double>(totalDistance, slope));
    }

    double gain= 0;
    double alt;
    double prevAlt = 0;
    foreach(alt, y)
    {
        gain += (alt > prevAlt) &&(prevAlt != 0) ? (alt - prevAlt) : 0;
        prevAlt = alt;
    }
    QMap<QString, QString> metrics;
    metrics[tr("Elevation Climbed")] = QString::number((int)gain);
    metrics[tr("Distance")] = QString::number(totalDistance,'f',1);
    metricsSummary->updateMetrics(metrics);

    plot->setData(x,y);
    plot->replot();

    update();
}

bool ImportPage::SaveWorkout()
{
    QString workoutDir = appsettings->value(this,GC_WORKOUTDIR).toString();
    QString filename = QFileDialog::getSaveFileName(this,QString(tr("Save Workout")),
                                                    workoutDir,tr("Computrainer Format *.crs"));
    if (filename.isEmpty()) return false;

    filename = WorkoutFileWriter::ensureSuffix(filename, QStringLiteral(".crs"));
    WorkoutFileWriter writer(filename);
    QString writeError;
    if (!writer.open(writeError)) {
        QMessageBox::critical(this, tr("Save Workout"),
                tr("Unable to save %1:\n%2").arg(filename, writeError));
        return false;
    }
    QTextStream &stream = writer.stream();
    // create the header
    SaveWorkoutHeader(stream, filename, QString("golden cheetah"), QString("DISTANCE GRADE WIND"));
    stream << "[COURSE DATA]" << Qt::endl;
    QPair<double,double> p;
    double prevDistance = 0;
    foreach (p,rideProfile)
    {
        // header indicates metric units, so convert accordingly
        double curDistance = p.first * (metricUnits ? 1.0 : KM_PER_MILE);
        stream << curDistance - prevDistance << " " << p.second <<" 0" << Qt::endl;
        prevDistance = curDistance;
    }
    stream << "[END COURSE DATA]" << Qt::endl;
    if (!writer.commit(writeError)) {
        QMessageBox::critical(this, tr("Save Workout"),
                tr("Unable to save %1:\n%2").arg(filename, writeError));
        return false;
    }

    // import them via the workoutimporter
    QStringList files;
    files << filename;
    const LibraryImportResult imported = Library::importFiles(
            context, files, LibraryBatchImportConfirmation::noDialog);
    if (!imported.allSucceeded()) {
        QMessageBox::warning(this, tr("Save Workout"),
                tr("The workout was saved but could not be added to the workout library."));
        return false;
    }
    return true;
}

GeneratedWorkoutPage::GeneratedWorkoutPage(Context *context, QWidget *parent)
    : WorkoutPage(context, parent)
{
}

void GeneratedWorkoutPage::initializePage()
{
    if (focusBox) return;

    setTitle(tr("Workout Generator"));
    setSubTitle(tr("Build a structured workout for a training goal"));

    focusBox = new QComboBox(this);
    focusBox->setObjectName(QStringLiteral("workoutGeneratorFocus"));
    focusBox->setAccessibleName(tr("Training focus"));
    for (WorkoutTrainingFocus focus : WorkoutGenerator::focuses()) {
        focusBox->addItem(WorkoutGenerator::focusName(focus), int(focus));
    }

    ftpBox = new QSpinBox(this);
    ftpBox->setObjectName(QStringLiteral("workoutGeneratorFtp"));
    ftpBox->setAccessibleName(tr("FTP"));
    ftpBox->setRange(50, 600);
    ftpBox->setSuffix(tr(" W"));

    auto createPowerControl = [this](
            const QString &name,
            QSlider *&slider,
            QSpinBox *&box,
            QLabel *&wattsValue,
            int minimum,
            int maximum) {
        QWidget *container = new QWidget(this);
        QHBoxLayout *layout = new QHBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        slider = new QSlider(Qt::Horizontal, container);
        slider->setObjectName(name + QStringLiteral("Slider"));
        slider->setRange(minimum, maximum);
        slider->setMinimumWidth(150 * dpiXFactor);
        box = new QSpinBox(container);
        box->setObjectName(name + QStringLiteral("Value"));
        box->setRange(minimum, maximum);
        box->setSuffix(tr(" %"));
        wattsValue = new QLabel(container);
        wattsValue->setObjectName(name + QStringLiteral("Watts"));
        wattsValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        wattsValue->setMinimumWidth(
                wattsValue->fontMetrics().horizontalAdvance(tr("1500 W")));
        layout->addWidget(slider, 1);
        layout->addWidget(box);
        layout->addWidget(wattsValue);
        connect(slider, &QSlider::valueChanged, box, &QSpinBox::setValue);
        connect(box, QOverload<int>::of(&QSpinBox::valueChanged),
                slider, &QSlider::setValue);
        return container;
    };

    QFormLayout *form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->addRow(tr("Training focus"), focusBox);
    form->addRow(tr("FTP"), ftpBox);
    form->addRow(tr("Work intensity"), createPowerControl(
            QStringLiteral("workoutGeneratorWorkPower"),
            workPowerSlider, workPowerBox, workPowerWattsValue, 20, 250));
    form->addRow(tr("Recovery intensity"), createPowerControl(
            QStringLiteral("workoutGeneratorRecoveryPower"),
            recoveryPowerSlider, recoveryPowerBox,
            recoveryPowerWattsValue, 20, 100));

    workSecondsBox = new QSpinBox(this);
    workSecondsBox->setObjectName(QStringLiteral("workoutGeneratorWorkSeconds"));
    workSecondsBox->setAccessibleName(tr("Work interval"));
    workSecondsBox->setRange(5, 2 * 60 * 60);
    workSecondsBox->setSuffix(tr(" s"));
    recoverySecondsBox = new QSpinBox(this);
    recoverySecondsBox->setObjectName(QStringLiteral("workoutGeneratorRecoverySeconds"));
    recoverySecondsBox->setAccessibleName(tr("Recovery interval"));
    recoverySecondsBox->setRange(0, 60 * 60);
    recoverySecondsBox->setSuffix(tr(" s"));
    repetitionsBox = new QSpinBox(this);
    repetitionsBox->setObjectName(QStringLiteral("workoutGeneratorRepetitions"));
    repetitionsBox->setAccessibleName(tr("Repetitions in first set"));
    repetitionsBox->setRange(1, 100);
    setsBox = new QSpinBox(this);
    setsBox->setObjectName(QStringLiteral("workoutGeneratorSets"));
    setsBox->setAccessibleName(tr("Sets"));
    setsBox->setRange(1, 20);
    repetitionDeltaBox = new QSpinBox(this);
    repetitionDeltaBox->setObjectName(QStringLiteral("workoutGeneratorRepetitionDelta"));
    repetitionDeltaBox->setAccessibleName(tr("Repetition change per set"));
    repetitionDeltaBox->setRange(-20, 20);
    repetitionDeltaBox->setToolTip(
            tr("Change in repetitions from one set to the next"));
    setRecoveryBox = new QSpinBox(this);
    setRecoveryBox->setObjectName(QStringLiteral("workoutGeneratorSetRecovery"));
    setRecoveryBox->setAccessibleName(tr("Recovery between sets"));
    setRecoveryBox->setRange(0, 60 * 60);
    setRecoveryBox->setSuffix(tr(" s"));
    finalSetRecoveryBox = new QSpinBox(this);
    finalSetRecoveryBox->setObjectName(
            QStringLiteral("workoutGeneratorFinalSetRecovery"));
    finalSetRecoveryBox->setAccessibleName(tr("Recovery before final set"));
    finalSetRecoveryBox->setRange(0, 60 * 60);
    finalSetRecoveryBox->setSuffix(tr(" s"));
    warmupMinutesBox = new QSpinBox(this);
    warmupMinutesBox->setObjectName(QStringLiteral("workoutGeneratorWarmup"));
    warmupMinutesBox->setAccessibleName(tr("Warm-up"));
    warmupMinutesBox->setRange(0, 60);
    warmupMinutesBox->setSuffix(tr(" min"));
    cooldownMinutesBox = new QSpinBox(this);
    cooldownMinutesBox->setObjectName(QStringLiteral("workoutGeneratorCooldown"));
    cooldownMinutesBox->setAccessibleName(tr("Cool-down"));
    cooldownMinutesBox->setRange(0, 60);
    cooldownMinutesBox->setSuffix(tr(" min"));
    recoverAfterLastBox = new QCheckBox(
            tr("Recovery after the last repetition"), this);
    recoverAfterLastBox->setObjectName(
            QStringLiteral("workoutGeneratorRecoverAfterLast"));

    workPowerSlider->setAccessibleName(tr("Work intensity slider"));
    workPowerBox->setAccessibleName(tr("Work intensity"));
    recoveryPowerSlider->setAccessibleName(tr("Recovery intensity slider"));
    recoveryPowerBox->setAccessibleName(tr("Recovery intensity"));

    form->addRow(tr("Work interval"), workSecondsBox);
    form->addRow(tr("Recovery interval"), recoverySecondsBox);
    form->addRow(tr("Repetitions in first set"), repetitionsBox);
    form->addRow(tr("Sets"), setsBox);
    form->addRow(tr("Repetition change per set"), repetitionDeltaBox);
    form->addRow(tr("Recovery between sets"), setRecoveryBox);
    form->addRow(tr("Recovery before final set"), finalSetRecoveryBox);
    form->addRow(tr("Warm-up"), warmupMinutesBox);
    form->addRow(tr("Warm-up start"), createPowerControl(
            QStringLiteral("workoutGeneratorWarmupStartPower"),
            warmupStartPowerSlider, warmupStartPowerBox,
            warmupStartPowerWattsValue, 20, 150));
    form->addRow(tr("Warm-up end"), createPowerControl(
            QStringLiteral("workoutGeneratorWarmupEndPower"),
            warmupEndPowerSlider, warmupEndPowerBox,
            warmupEndPowerWattsValue, 20, 150));
    warmupStartPowerSlider->setAccessibleName(tr("Warm-up start slider"));
    warmupStartPowerBox->setAccessibleName(tr("Warm-up start"));
    warmupEndPowerSlider->setAccessibleName(tr("Warm-up end slider"));
    warmupEndPowerBox->setAccessibleName(tr("Warm-up end"));
    primerSecondsBox = new QSpinBox(this);
    primerSecondsBox->setObjectName(
            QStringLiteral("workoutGeneratorPrimerSeconds"));
    primerSecondsBox->setAccessibleName(tr("Warm-up primer"));
    primerSecondsBox->setRange(0, 20 * 60);
    primerSecondsBox->setSingleStep(15);
    primerSecondsBox->setAccelerated(true);
    primerSecondsBox->setSuffix(tr(" s"));
    form->addRow(tr("Warm-up primer"), primerSecondsBox);
    form->addRow(tr("Primer intensity"), createPowerControl(
            QStringLiteral("workoutGeneratorPrimerPower"),
            primerPowerSlider, primerPowerBox,
            primerPowerWattsValue, 20, 250));
    primerPowerSlider->setAccessibleName(tr("Primer intensity slider"));
    primerPowerBox->setAccessibleName(tr("Primer intensity"));
    preWorkRecoverySecondsBox = new QSpinBox(this);
    preWorkRecoverySecondsBox->setObjectName(
            QStringLiteral("workoutGeneratorPreWorkRecoverySeconds"));
    preWorkRecoverySecondsBox->setAccessibleName(
            tr("Recovery before main set"));
    preWorkRecoverySecondsBox->setRange(0, 30 * 60);
    preWorkRecoverySecondsBox->setSingleStep(15);
    preWorkRecoverySecondsBox->setAccelerated(true);
    preWorkRecoverySecondsBox->setSuffix(tr(" s"));
    form->addRow(tr("Recovery before main set"),
                 preWorkRecoverySecondsBox);
    form->addRow(tr("Cool-down"), cooldownMinutesBox);
    form->addRow(QString(), recoverAfterLastBox);

    QWidget *controls = new QWidget(this);
    controls->setLayout(form);
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(controls);
    scroll->setMinimumWidth(500 * dpiXFactor);

    plot = new WorkoutPlot();
    plot->setObjectName(QStringLiteral("workoutGeneratorPreview"));
    plot->setYAxisTitle(tr("% FTP"));
    plot->setXAxisTitle(tr("Time (minutes)"));
    plot->setMinimumSize(480 * dpiXFactor, 280 * dpiYFactor);

    QGridLayout *summary = new QGridLayout();
    durationValue = new QLabel(this);
    averagePowerValue = new QLabel(this);
    stressValue = new QLabel(this);
    intervalValue = new QLabel(this);
    intervalValue->setWordWrap(true);
    durationValue->setAccessibleDescription(tr("Generated duration"));
    averagePowerValue->setAccessibleDescription(
            tr("Generated average power"));
    stressValue->setAccessibleDescription(tr("Generated estimated stress"));
    intervalValue->setAccessibleDescription(tr("Generated main set"));
    summary->addWidget(new QLabel(tr("Duration"), this), 0, 0);
    summary->addWidget(durationValue, 0, 1);
    summary->addWidget(new QLabel(tr("Average power"), this), 1, 0);
    summary->addWidget(averagePowerValue, 1, 1);
    summary->addWidget(new QLabel(tr("Estimated stress"), this), 2, 0);
    summary->addWidget(stressValue, 2, 1);
    summary->addWidget(new QLabel(tr("Main set"), this), 3, 0);
    summary->addWidget(intervalValue, 3, 1);

    validationLabel = new QLabel(this);
    validationLabel->setObjectName(QStringLiteral("workoutGeneratorValidation"));
    validationLabel->setAccessibleDescription(tr("Workout validation error"));
    validationLabel->setWordWrap(true);
    QPalette validationPalette = validationLabel->palette();
    validationPalette.setColor(QPalette::WindowText, Qt::red);
    validationLabel->setPalette(validationPalette);

    QVBoxLayout *previewLayout = new QVBoxLayout();
    previewLayout->addWidget(plot, 1);
    previewLayout->addLayout(summary);
    previewLayout->addWidget(validationLabel);

    QHBoxLayout *pageLayout = new QHBoxLayout(this);
    pageLayout->addWidget(scroll);
    pageLayout->addLayout(previewLayout, 1);

    connect(focusBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &GeneratedWorkoutPage::focusChanged);
    const QList<QSpinBox *> boxes = {
        ftpBox, workPowerBox, recoveryPowerBox, workSecondsBox,
        recoverySecondsBox, repetitionsBox, setsBox, repetitionDeltaBox,
        setRecoveryBox, finalSetRecoveryBox,
        warmupMinutesBox, warmupStartPowerBox, warmupEndPowerBox,
        primerSecondsBox, primerPowerBox, preWorkRecoverySecondsBox,
        cooldownMinutesBox
    };
    for (QSpinBox *box : boxes) {
        connect(box, QOverload<int>::of(&QSpinBox::valueChanged),
                this, &GeneratedWorkoutPage::controlsChanged);
    }
    connect(recoverAfterLastBox, &QCheckBox::toggled,
            this, &GeneratedWorkoutPage::controlsChanged);

    const QList<QWidget *> tabOrder = {
        focusBox, ftpBox,
        workPowerSlider, workPowerBox,
        recoveryPowerSlider, recoveryPowerBox,
        workSecondsBox, recoverySecondsBox,
        repetitionsBox, setsBox, repetitionDeltaBox,
        setRecoveryBox, finalSetRecoveryBox,
        warmupMinutesBox,
        warmupStartPowerSlider, warmupStartPowerBox,
        warmupEndPowerSlider, warmupEndPowerBox,
        primerSecondsBox,
        primerPowerSlider, primerPowerBox,
        preWorkRecoverySecondsBox,
        cooldownMinutesBox,
        recoverAfterLastBox
    };
    for (int index = 1; index < tabOrder.size(); ++index) {
        QWidget::setTabOrder(tabOrder.at(index - 1), tabOrder.at(index));
    }

    int ftp = 190;
    if (context && context->athlete && context->athlete->zones("Bike")) {
        const int range = context->athlete->zones("Bike")->whichRange(
                QDate::currentDate());
        if (range >= 0) ftp = context->athlete->zones("Bike")->getCP(range);
    }
    settings = WorkoutGenerator::defaultsFor(
            WorkoutTrainingFocus::AnaerobicCapacity20_20);
    settings.ftpWatts = ftp;
    applySettings(settings);
}

WorkoutGenerationSettings GeneratedWorkoutPage::settingsFromControls() const
{
    WorkoutGenerationSettings value = settings;
    value.focus = WorkoutTrainingFocus(focusBox->currentData().toInt());
    value.ftpWatts = ftpBox->value();
    value.workPercentFtp = workPowerBox->value();
    value.recoveryPercentFtp = recoveryPowerBox->value();
    value.workSeconds = workSecondsBox->value();
    value.recoverySeconds = recoverySecondsBox->value();
    value.repetitionsPerBlock = repetitionsBox->value();
    value.blockCount = setsBox->value();
    value.repetitionDeltaPerBlock = repetitionDeltaBox->value();
    value.blockRecoverySeconds = setRecoveryBox->value();
    value.lastBlockRecoverySeconds = finalSetRecoveryBox->value();
    value.warmupSeconds = warmupMinutesBox->value() * 60;
    value.warmupStartPercentFtp = warmupStartPowerBox->value();
    value.warmupEndPercentFtp = warmupEndPowerBox->value();
    value.primerSeconds = primerSecondsBox->value();
    value.primerPercentFtp = primerPowerBox->value();
    value.preWorkRecoverySeconds = preWorkRecoverySecondsBox->value();
    value.cooldownSeconds = cooldownMinutesBox->value() * 60;
    value.includeRecoveryAfterLastRep = recoverAfterLastBox->isChecked();
    return value;
}

void GeneratedWorkoutPage::applySettings(
        const WorkoutGenerationSettings &value)
{
    settings = value;
    const QList<QObject *> controls = {
        focusBox, ftpBox,
        workPowerSlider, workPowerBox,
        recoveryPowerSlider, recoveryPowerBox, workSecondsBox,
        recoverySecondsBox, repetitionsBox, setsBox, repetitionDeltaBox,
        setRecoveryBox, finalSetRecoveryBox,
        warmupMinutesBox,
        warmupStartPowerSlider, warmupStartPowerBox,
        warmupEndPowerSlider, warmupEndPowerBox,
        primerSecondsBox,
        primerPowerSlider, primerPowerBox,
        preWorkRecoverySecondsBox,
        cooldownMinutesBox,
        recoverAfterLastBox
    };
    std::vector<std::unique_ptr<QSignalBlocker>> blockers;
    blockers.reserve(controls.size());
    for (QObject *control : controls) {
        blockers.push_back(std::make_unique<QSignalBlocker>(control));
    }

    const int focusIndex = focusBox->findData(int(value.focus));
    if (focusIndex >= 0) focusBox->setCurrentIndex(focusIndex);
    ftpBox->setValue(value.ftpWatts);
    workPowerBox->setValue(int(std::lround(value.workPercentFtp)));
    workPowerSlider->setValue(workPowerBox->value());
    recoveryPowerBox->setValue(int(std::lround(value.recoveryPercentFtp)));
    recoveryPowerSlider->setValue(recoveryPowerBox->value());
    workSecondsBox->setValue(value.workSeconds);
    recoverySecondsBox->setValue(value.recoverySeconds);
    repetitionsBox->setValue(value.repetitionsPerBlock);
    setsBox->setValue(value.blockCount);
    repetitionDeltaBox->setValue(value.repetitionDeltaPerBlock);
    setRecoveryBox->setValue(value.blockRecoverySeconds);
    finalSetRecoveryBox->setValue(value.lastBlockRecoverySeconds);
    warmupMinutesBox->setValue(value.warmupSeconds / 60);
    warmupStartPowerBox->setValue(
            int(std::lround(value.warmupStartPercentFtp)));
    warmupStartPowerSlider->setValue(warmupStartPowerBox->value());
    warmupEndPowerBox->setValue(
            int(std::lround(value.warmupEndPercentFtp)));
    warmupEndPowerSlider->setValue(warmupEndPowerBox->value());
    primerSecondsBox->setValue(value.primerSeconds);
    primerPowerBox->setValue(int(std::lround(value.primerPercentFtp)));
    primerPowerSlider->setValue(primerPowerBox->value());
    preWorkRecoverySecondsBox->setValue(value.preWorkRecoverySeconds);
    cooldownMinutesBox->setValue(value.cooldownSeconds / 60);
    recoverAfterLastBox->setChecked(value.includeRecoveryAfterLastRep);
    controlsChanged();
}

void GeneratedWorkoutPage::focusChanged(int index)
{
    if (index < 0) return;
    WorkoutGenerationSettings value = WorkoutGenerator::defaultsFor(
            WorkoutTrainingFocus(focusBox->itemData(index).toInt()));
    value.ftpWatts = ftpBox->value();
    applySettings(value);
}

void GeneratedWorkoutPage::controlsChanged()
{
    if (!focusBox) return;
    settings = settingsFromControls();
    workPowerWattsValue->setText(tr("%1 W").arg(
            WorkoutGenerator::wattsForPercent(
                    settings.ftpWatts, settings.workPercentFtp)));
    recoveryPowerWattsValue->setText(tr("%1 W").arg(
            WorkoutGenerator::wattsForPercent(
                    settings.ftpWatts, settings.recoveryPercentFtp)));
    warmupStartPowerWattsValue->setText(tr("%1 W").arg(
            WorkoutGenerator::wattsForPercent(
                    settings.ftpWatts, settings.warmupStartPercentFtp)));
    warmupEndPowerWattsValue->setText(tr("%1 W").arg(
            WorkoutGenerator::wattsForPercent(
                    settings.ftpWatts, settings.warmupEndPercentFtp)));
    primerPowerWattsValue->setText(tr("%1 W").arg(
            WorkoutGenerator::wattsForPercent(
                    settings.ftpWatts, settings.primerPercentFtp)));
    generated = WorkoutGenerator::generate(settings);
    const bool ready = generated.status == WorkoutGenerationStatus::Ready;
    validationLabel->setVisible(!ready);
    validationLabel->setText(ready ? QString() : generated.error);

    QVector<double> x;
    QVector<double> y;
    double minutes = 0.0;
    if (ready) {
        x.reserve(int(generated.intervals.size()) * 2);
        y.reserve(int(generated.intervals.size()) * 2);
        for (const WorkoutGeneratedInterval &interval : generated.intervals) {
            x.append(minutes);
            y.append(interval.startPercentFtp);
            minutes += double(interval.durationSeconds) / 60.0;
            x.append(minutes);
            y.append(interval.endPercentFtp);
        }
    }
    plot->setAxisAutoScale(QwtAxis::YLeft);
    plot->setAxisAutoScale(QwtAxis::XBottom);
    plot->setData(x, y);
    plot->replot();

    const int duration = ready ? generated.summary.durationSeconds : 0;
    durationValue->setText(QStringLiteral("%1:%2:%3")
            .arg(duration / 3600)
            .arg((duration / 60) % 60, 2, 10, QLatin1Char('0'))
            .arg(duration % 60, 2, 10, QLatin1Char('0')));
    averagePowerValue->setText(ready
            ? tr("%1 W (%2% FTP)")
                .arg(int(std::lround(generated.summary.averageWatts)))
                .arg(generated.summary.averagePercentFtp, 0, 'f', 1)
            : QStringLiteral("-"));
    stressValue->setText(ready
            ? QString::number(generated.summary.estimatedStress, 'f', 1)
            : QStringLiteral("-"));
    if (ready) {
        QStringList repetitions;
        for (int value : generated.summary.repetitionsByBlock) {
            repetitions.append(QString::number(value));
        }
        QStringList recoveries;
        const int setCount = generated.summary.repetitionsByBlock.size();
        for (int set = 0; set + 1 < setCount; ++set) {
            const int seconds = set + 2 == setCount
                    ? settings.lastBlockRecoverySeconds
                    : settings.blockRecoverySeconds;
            recoveries.append(QStringLiteral("%1:%2")
                    .arg(seconds / 60)
                    .arg(seconds % 60, 2, 10, QLatin1Char('0')));
        }
        intervalValue->setText(recoveries.isEmpty()
                ? tr("%1 efforts").arg(repetitions.join(QStringLiteral(" / ")))
                : tr("%1 efforts; set recovery %2")
                    .arg(repetitions.join(QStringLiteral(" / ")),
                         recoveries.join(QStringLiteral(" / "))));
    } else {
        intervalValue->setText(QStringLiteral("-"));
    }
    emit completeChanged();
}

bool GeneratedWorkoutPage::isComplete() const
{
    return generated.status == WorkoutGenerationStatus::Ready;
}

bool GeneratedWorkoutPage::SaveWorkout()
{
    generated = WorkoutGenerator::generate(settingsFromControls());
    if (generated.status != WorkoutGenerationStatus::Ready) {
        QMessageBox::warning(this, tr("Save Workout"), generated.error);
        return false;
    }

    QString workoutDir = appsettings->value(this, GC_WORKOUTDIR).toString();
    QString filename = QFileDialog::getSaveFileName(
            this, tr("Save Generated Workout"), workoutDir,
            tr("Relative Power Workout *.mrc"));
    if (filename.isEmpty()) return false;
    filename = WorkoutFileWriter::ensureSuffix(filename, QStringLiteral(".mrc"));

    WorkoutFileWriter writer(filename);
    QString error;
    if (!writer.open(error)) {
        QMessageBox::critical(this, tr("Save Workout"),
                tr("Unable to save %1:\n%2").arg(filename, error));
        return false;
    }
    QTextStream &stream = writer.stream();
    SaveWorkoutHeader(stream, QFileInfo(filename).fileName(),
            WorkoutGenerator::focusName(settings.focus),
            QStringLiteral("MINUTES PERCENT"));
    stream << QString::fromUtf8(WorkoutGenerator::mrcCourseData(generated));
    if (!writer.commit(error)) {
        QMessageBox::critical(this, tr("Save Workout"),
                tr("Unable to save %1:\n%2").arg(filename, error));
        return false;
    }

    const LibraryImportResult imported = Library::importFiles(
            context, {filename}, LibraryBatchImportConfirmation::noDialog);
    if (!imported.allSucceeded()) {
        QMessageBox::warning(this, tr("Save Workout"),
                tr("The workout was saved but could not be added to the workout library."));
        return false;
    }
    return true;
}

WorkoutWizard::WorkoutWizard(Context *context) :QWizard(context->mainWindow)
{
    HelpWhatsThis *help = new HelpWhatsThis(this);
    this->setWhatsThis(help->getWhatsThisText(HelpWhatsThis::MenuBar_Tools_CreateWorkout));
    setMinimumSize(QSize(600*dpiXFactor, 500 *dpiYFactor));
    resize(QSize(1120*dpiXFactor, 700*dpiYFactor));

    setAttribute(Qt::WA_DeleteOnClose);
    setPage(WW_WorkoutTypePage, new WorkoutTypePage(context));
    setPage(WW_GeneratedWorkoutPage, new GeneratedWorkoutPage(context));
    setPage(WW_AbsWattagePage, new AbsWattagePage(context));
    setPage(WW_RelWattagePage, new RelWattagePage(context));
    setPage(WW_GradientPage, new GradientPage(context));
    setPage(WW_ImportPage, new ImportPage(context));
    this->setStartId(WW_WorkoutTypePage);

#ifdef Q_OS_MAC
    setWizardStyle(QWizard::MacStyle);
#else
    setWizardStyle(QWizard::ModernStyle);
#endif
    setWindowTitle(tr("Workout Wizard"));
}
// called at the end of the wizard...
void WorkoutWizard::accept()
{
    WorkoutPage *page = (WorkoutPage *)this->currentPage();
    if (page->SaveWorkout()) QWizard::accept();
}
