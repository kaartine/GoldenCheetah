#include <QtTest>

#include "Train/VMProWidget.h"

#include <QPointer>

#include <memory>

namespace {

QLowEnergyService *service(quintptr identity)
{
    return reinterpret_cast<QLowEnergyService *>(identity);
}

qulonglong connectedService(const VMProWidget *widget)
{
    return widget->property("testService").toULongLong();
}

} // namespace

class TestVMProWidgetLifecycle : public QObject
{
    Q_OBJECT

private slots:
    void sameDeviceReconnectsExistingWidget();
    void devicesOwnIndependentWidgets();
    void destroyingDeviceClearsOnlyItsWidget();
};

void TestVMProWidgetLifecycle::sameDeviceReconnectsExistingWidget()
{
    QObject owner;
    QPointer<VMProWidget> widget;

    VMProWidget *created = VMProWidget::createOrReconnect(
        widget, service(1), &owner);

    QVERIFY(created);
    QCOMPARE(widget.data(), created);
    QCOMPARE(created->parent(), &owner);
    QCOMPARE(connectedService(created), qulonglong(1));

    VMProWidget *reconnected = VMProWidget::createOrReconnect(
        widget, service(2), &owner);

    QCOMPARE(reconnected, created);
    QCOMPARE(widget.data(), created);
    QCOMPARE(connectedService(created), qulonglong(2));
}

void TestVMProWidgetLifecycle::devicesOwnIndependentWidgets()
{
    QObject firstOwner;
    QObject secondOwner;
    QPointer<VMProWidget> firstWidget;
    QPointer<VMProWidget> secondWidget;

    VMProWidget *first = VMProWidget::createOrReconnect(
        firstWidget, service(1), &firstOwner);
    VMProWidget *second = VMProWidget::createOrReconnect(
        secondWidget, service(2), &secondOwner);

    QVERIFY(first);
    QVERIFY(second);
    QVERIFY(first != second);
    QCOMPARE(first->parent(), &firstOwner);
    QCOMPARE(second->parent(), &secondOwner);
    QCOMPARE(connectedService(first), qulonglong(1));
    QCOMPARE(connectedService(second), qulonglong(2));
}

void TestVMProWidgetLifecycle::destroyingDeviceClearsOnlyItsWidget()
{
    std::unique_ptr<QObject> firstOwner(new QObject);
    std::unique_ptr<QObject> secondOwner(new QObject);
    QPointer<VMProWidget> firstWidget;
    QPointer<VMProWidget> secondWidget;

    VMProWidget::createOrReconnect(
        firstWidget, service(1), firstOwner.get());
    VMProWidget::createOrReconnect(
        secondWidget, service(2), secondOwner.get());

    firstOwner.reset();

    QVERIFY(firstWidget.isNull());
    QVERIFY(!secondWidget.isNull());
    QCOMPARE(secondWidget->parent(), secondOwner.get());
    QCOMPARE(connectedService(secondWidget), qulonglong(2));

    QObject replacementOwner;
    VMProWidget *replacement = VMProWidget::createOrReconnect(
        firstWidget, service(3), &replacementOwner);

    QVERIFY(replacement);
    QVERIFY(replacement != secondWidget.data());
    QCOMPARE(firstWidget.data(), replacement);
    QCOMPARE(connectedService(replacement), qulonglong(3));
}

QTEST_MAIN(TestVMProWidgetLifecycle)
#include "testVMProWidgetLifecycle.moc"
