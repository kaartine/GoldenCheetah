/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <QtTest>
#include <QCompleter>
#include <QPointer>
#include <QPropertyAnimation>
#include <QStyle>
#include <QTableWidget>
#include <QStringListModel>
#include <QTableView>
#include <QTemporaryDir>
#include <memory>

#include "CloudService.h"
#include "Colors.h"
#include "Context.h"
#include "RideEditor.h"
#include "RideFileTableModel.h"
#include "Settings.h"
#include "TagBar.h"
#include "TrainDB.h"
#include "WorkoutTagWrapper.h"
#include "LTMPopup.h"
#include "SharedStyle.h"
#include "qxtstringspinbox.h"

class TestWidgetOwnership : public QObject
{
    Q_OBJECT

    static QTableView *rideTable(RideEditor &editor)
    {
        for (auto *table : editor.findChildren<QTableView *>()) {
            if (qobject_cast<RideFileTableModel *>(table->model())) return table;
        }
        return nullptr;
    }

private slots:
    void initTestCase()
    {
        // Production static initialization precedes main. The isolated launcher
        // must set fresh paths before execution, not merely inside this test.
        QVERIFY2(!qEnvironmentVariableIsEmpty("GC_CHART_OWNERSHIP_ROOT"),
                 "Use the isolated chartOwnership launcher");
        const QString root = qEnvironmentVariable("GC_CHART_OWNERSHIP_ROOT");
        QCOMPARE(qEnvironmentVariable("XDG_CONFIG_HOME"), root + "/config");
        QVERIFY(appsettings);
        GCColor::setupColors();
    }

    void rideEditorDeletesModel()
    {
        Context context(nullptr);
        auto editor = std::make_unique<RideEditor>(&context);
        auto *table = rideTable(*editor);
        QVERIFY(table);
        QPointer<QAbstractItemModel> model(table->model());
        QVERIFY(model);
        QCOMPARE(model->parent(), editor.get());
        editor.reset();
        QVERIFY(model.isNull());
    }

    void rideEditorDeletesDelegate()
    {
        Context context(nullptr);
        auto editor = std::make_unique<RideEditor>(&context);
        auto *table = rideTable(*editor);
        QVERIFY(table);
        QPointer<CellDelegate> delegate(
            qobject_cast<CellDelegate *>(table->itemDelegate()));
        QVERIFY(delegate);
        QCOMPARE(delegate->parent(), editor.get());
        editor.reset();
        QVERIFY(delegate.isNull());
    }

    void cloudDownloadWidgetDeletesAnimation()
    {
        Context context(nullptr);
        auto widget = std::make_unique<CloudServiceAutoDownloadWidget>(&context, nullptr);
        const auto animations = widget->findChildren<QPropertyAnimation *>(
            QString(), Qt::FindDirectChildrenOnly);
        QCOMPARE(animations.size(), 1);
        QPointer<QPropertyAnimation> animation(animations.first());
        QCOMPARE(animation->targetObject(), widget.get());
        QCOMPARE(animation->propertyName(), QByteArray("transition"));
        // No download or network operation is started by this ownership test.
        widget.reset();
        QVERIFY(animation.isNull());
    }

    void tagBarReusesAndDeletesCompletionModel()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        TrainDB database(QDir(temporary.path()), "widget-ownership-tags", false);
        QCOMPARE(database.schemaStatus(), TrainDB::SchemaStatus::current);
        QVERIFY(database.addTag("zulu") != TAGSTORE_UNDEFINED_ID);
        QVERIFY(database.addTag("alpha") != TAGSTORE_UNDEFINED_ID);

        // The real wrapper's empty filepath represents no workout/tags and
        // does not access the process-global training database.
        WorkoutTagWrapper workout;
        auto bar = std::make_unique<TagBar>(&database, QColor(Qt::black));
        auto *edit = bar->findChild<QLineEdit *>();
        QVERIFY(edit);
        QPointer<QCompleter> completer(edit->completer());
        QVERIFY(completer);
        QPointer<QStringListModel> model(
            qobject_cast<QStringListModel *>(completer->model()));
        QVERIFY(model);
        QCOMPARE(model->parent(), completer.data());

        bar->setTaggable(&workout);
        QVERIFY2(model, "Refreshing tags must retain the completer-owned model");
        QCOMPARE(completer->model(), model.data());
        QCOMPARE(model->stringList(), QStringList({"alpha", "zulu"}));
        const int beta = database.addTag("beta");
        QVERIFY(beta != TAGSTORE_UNDEFINED_ID);
        bar->tagStoreChanged(beta, TAGSTORE_UNDEFINED_ID, TAGSTORE_UNDEFINED_ID);
        bar->setTaggable(&workout);
        QCOMPARE(completer->model(), model.data());
        QCOMPARE(model->stringList(), QStringList({"alpha", "beta", "zulu"}));
        bar.reset();
        QVERIFY(completer.isNull());
        QVERIFY(model.isNull());
    }

    void stringSpinBoxDeletesOwnStyle()
    {
        auto spin = std::make_unique<QxtStringSpinBox>(nullptr);
        QPointer<QStyle> style(spin->style());
        QVERIFY(style);
        QVERIFY2(style != QApplication::style(), "Spin box must use its own style instance");
        QCOMPARE(style->name(), QStringLiteral("fusion"));
        spin.reset();
        const bool deleted = style.isNull();
        delete style.data(); // Only needed while exercising the old object.
        QVERIFY2(deleted, "Spin box must release the style it creates");
    }

    void sharedFusionStyleIsApplicationOwned()
    {
        QPointer<QStyle> shared(sharedFusionStyle());
        QVERIFY(shared);
        QCOMPARE(shared->parent(), static_cast<QObject *>(qApp));
        QCOMPARE(shared->name(), QStringLiteral("fusion"));
        QCOMPARE(sharedFusionStyle(), shared.data());

        Context context(nullptr);
        for (int i = 0; i < 2; ++i) {
            auto popup = std::make_unique<LTMPopup>(&context);
            QVERIFY(popup->findChild<QTableWidget *>());
            popup.reset();
            QVERIFY2(shared, "Destroying a user must not delete the shared style");
        }
        QCOMPARE(sharedFusionStyle(), shared.data());
    }

    void sharedFusionStyleIsRecreatedAfterDeletion()
    {
        QPointer<QStyle> first(sharedFusionStyle());
        QVERIFY(first);
        delete first.data();
        QVERIFY(first.isNull());
        QPointer<QStyle> second(sharedFusionStyle());
        QVERIFY(second);
        QCOMPARE(second->parent(), static_cast<QObject *>(qApp));
        QCOMPARE(sharedFusionStyle(), second.data());
    }
};

QTEST_MAIN(TestWidgetOwnership)
#include "testWidgetOwnership.moc"
