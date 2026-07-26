/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <QtTest>

#include <memory>

#include "Core/DataFilter.h"
#include "Core/DataFilterZones.h"

extern QStringList DataFiltererrors;
extern Leaf *DataFilterroot;
extern int DataFilterparse();
extern void DataFilter_setString(QString);
extern void DataFilter_clearString();

static void destroyTree(Leaf *leaf)
{
    if (!leaf) return;
    leaf->clear(leaf);
    delete leaf;
}

using LeafPtr = std::unique_ptr<Leaf, decltype(&destroyTree)>;

static LeafPtr parseFormula(const QString &formula, int *status)
{
    DataFiltererrors.clear();
    DataFilterroot = nullptr;
    DataFilter_setString(formula);
    *status = DataFilterparse();
    DataFilter_clearString();
    return LeafPtr(DataFilterroot, &destroyTree);
}

static Leaf *nestedZones(Leaf *root)
{
    if (!root || root->type != Leaf::Function || root->fparms.isEmpty()) {
        return nullptr;
    }
    Leaf *nested = root->fparms.first();
    return nested->type == Leaf::Function
            && nested->function == QStringLiteral("zones")
        ? nested
        : nullptr;
}

static Leaf *invalidZones()
{
    Leaf *zones = new Leaf(0, 0);
    zones->type = Leaf::Function;
    zones->function = QStringLiteral("zones");

    Leaf *series = new Leaf(0, 0);
    series->type = Leaf::Symbol;
    series->lvalue.n = new QString(QStringLiteral("WATTS"));

    Leaf *field = new Leaf(0, 0);
    field->type = Leaf::Symbol;
    field->lvalue.n = new QString(QStringLiteral("NAME"));

    zones->fparms << series << field;
    return zones;
}

class TestDataFilterZones : public QObject
{
    Q_OBJECT

private slots:
    void rootMixedCaseCallIsCanonicalized();
    void nestedMixedCaseCallIsCanonicalized();
    void unknownRootLiteralIsRejected_data();
    void unknownRootLiteralIsRejected();
    void nestedUnknownLiteralMarksNestedLeaf();
    void malformedCallIsRejected_data();
    void malformedCallIsRejected();
    void malformedAstIsRejected_data();
    void malformedAstIsRejected();
    void treeValidationCanonicalizesSkippedNestedCall();
    void treeValidationRejectsSkippedNestedCall();
    void treeValidationCoversOwnedChildren_data();
    void treeValidationCoversOwnedChildren();
    void evaluationArgumentsCanonicalizeWithoutValidation();
    void functionDefinitionParses();
    void parserErrorAfterFunctionReductionCleansTree();
};

void TestDataFilterZones::rootMixedCaseCallIsCanonicalized()
{
    int status = -1;
    LeafPtr root = parseFormula(QStringLiteral("zones(POWER,NAME)"), &status);
    QCOMPARE(status, 0);
    QVERIFY(root);

    QVERIFY(DataFilterZones::validate(root.get()));
    QVERIFY(!root->inerror);
    QCOMPARE(*root->fparms.at(0)->lvalue.n, QStringLiteral("power"));
    QCOMPARE(*root->fparms.at(1)->lvalue.n, QStringLiteral("name"));
}

void TestDataFilterZones::nestedMixedCaseCallIsCanonicalized()
{
    int status = -1;
    LeafPtr root = parseFormula(
        QStringLiteral("sum(zones(FATIGUE,DESCRIPTION))"), &status);
    QCOMPARE(status, 0);
    QVERIFY(root);
    Leaf *zones = nestedZones(root.get());
    QVERIFY(zones);

    QVERIFY(DataFilterZones::validate(zones));
    QVERIFY(!root->inerror);
    QVERIFY(!zones->inerror);
    QCOMPARE(*zones->fparms.at(0)->lvalue.n, QStringLiteral("fatigue"));
    QCOMPARE(*zones->fparms.at(1)->lvalue.n,
             QStringLiteral("description"));
}

void TestDataFilterZones::unknownRootLiteralIsRejected_data()
{
    QTest::addColumn<QString>("formula");

    QTest::newRow("series")
        << QStringLiteral("zones(WATTS,NAME)");
    QTest::newRow("field")
        << QStringLiteral("zones(POWER,COLOUR)");
}

void TestDataFilterZones::unknownRootLiteralIsRejected()
{
    QFETCH(QString, formula);
    int status = -1;
    LeafPtr root = parseFormula(formula, &status);
    QCOMPARE(status, 0);
    QVERIFY(root);

    QVERIFY(!DataFilterZones::validate(root.get()));
    QVERIFY(root->inerror);
}

void TestDataFilterZones::nestedUnknownLiteralMarksNestedLeaf()
{
    int status = -1;
    LeafPtr root = parseFormula(
        QStringLiteral("sum(zones(WATTS,NAME))"), &status);
    QCOMPARE(status, 0);
    QVERIFY(root);
    Leaf *zones = nestedZones(root.get());
    QVERIFY(zones);

    QVERIFY(!DataFilterZones::validate(zones));
    QVERIFY(zones->inerror);
    QVERIFY(!root->inerror);
}

void TestDataFilterZones::malformedCallIsRejected_data()
{
    QTest::addColumn<QString>("formula");

    QTest::newRow("arity")
        << QStringLiteral("zones(POWER)");
    QTest::newRow("type")
        << QStringLiteral("zones(POWER,1)");
}

void TestDataFilterZones::malformedCallIsRejected()
{
    QFETCH(QString, formula);
    int status = -1;
    LeafPtr root = parseFormula(formula, &status);
    QCOMPARE(status, 0);
    QVERIFY(root);

    QVERIFY(!DataFilterZones::validate(root.get()));
    QVERIFY(root->inerror);
}

void TestDataFilterZones::malformedAstIsRejected_data()
{
    QTest::addColumn<bool>("nullParameter");

    QTest::newRow("null-parameter") << true;
    QTest::newRow("null-symbol-value") << false;
}

void TestDataFilterZones::malformedAstIsRejected()
{
    QFETCH(bool, nullParameter);

    LeafPtr root(new Leaf(0, 0), &destroyTree);
    root->type = Leaf::Function;
    root->function = QStringLiteral("zones");

    Leaf *series = new Leaf(0, 0);
    series->type = Leaf::Symbol;
    if (!nullParameter) {
        series->lvalue.n = nullptr;
    }

    Leaf *field = new Leaf(0, 0);
    field->type = Leaf::Symbol;
    field->lvalue.n = new QString(QStringLiteral("name"));

    root->fparms << (nullParameter ? nullptr : series) << field;
    if (nullParameter) delete series;

    QVERIFY(!DataFilterZones::arguments(root.get()).valid);
    QVERIFY(!DataFilterZones::validate(root.get()));
    QVERIFY(root->inerror);
}

void TestDataFilterZones::treeValidationCanonicalizesSkippedNestedCall()
{
    int status = -1;
    LeafPtr root = parseFormula(
        QStringLiteral("cumsum(zones(POWER,NAME))"), &status);
    QCOMPARE(status, 0);
    QVERIFY(root);
    Leaf *zones = nestedZones(root.get());
    QVERIFY(zones);

    QCOMPARE(DataFilterZones::validateTree(root.get()), 0);
    QVERIFY(!zones->inerror);
    QCOMPARE(*zones->fparms.at(0)->lvalue.n, QStringLiteral("power"));
    QCOMPARE(*zones->fparms.at(1)->lvalue.n, QStringLiteral("name"));
}

void TestDataFilterZones::treeValidationRejectsSkippedNestedCall()
{
    int status = -1;
    LeafPtr root = parseFormula(
        QStringLiteral("cumsum(zones(WATTS,NAME))"), &status);
    QCOMPARE(status, 0);
    QVERIFY(root);
    Leaf *zones = nestedZones(root.get());
    QVERIFY(zones);

    QCOMPARE(DataFilterZones::validateTree(root.get()), 1);
    QVERIFY(zones->inerror);
    QVERIFY(!root->inerror);
    QCOMPARE(DataFilterZones::validateTree(root.get()), 0);
}

void TestDataFilterZones::treeValidationCoversOwnedChildren_data()
{
    QTest::addColumn<int>("shape");

    QTest::newRow("logical") << 0;
    QTest::newRow("binary-operation") << 1;
    QTest::newRow("operation") << 2;
    QTest::newRow("unary-operation") << 3;
    QTest::newRow("function-value") << 4;
    QTest::newRow("function-series") << 5;
    QTest::newRow("function-parameter") << 6;
    QTest::newRow("compound") << 7;
    QTest::newRow("conditional-value") << 8;
    QTest::newRow("conditional-alternative") << 9;
    QTest::newRow("conditional-condition") << 10;
    QTest::newRow("index-value") << 11;
    QTest::newRow("index-parameter") << 12;
    QTest::newRow("select-value") << 13;
    QTest::newRow("select-parameter") << 14;
}

void TestDataFilterZones::treeValidationCoversOwnedChildren()
{
    QFETCH(int, shape);

    LeafPtr root(new Leaf(0, 0), &destroyTree);
    Leaf *zones = invalidZones();

    switch (shape) {
    case 0:
        root->type = Leaf::Logical;
        root->lvalue.l = zones;
        break;
    case 1:
        root->type = Leaf::BinaryOperation;
        root->lvalue.l = zones;
        break;
    case 2:
        root->type = Leaf::Operation;
        root->rvalue.l = zones;
        break;
    case 3:
        root->type = Leaf::UnaryOperation;
        root->lvalue.l = zones;
        break;
    case 4:
        root->type = Leaf::Function;
        root->function = QStringLiteral("wrapper");
        root->lvalue.l = zones;
        break;
    case 5:
        root->type = Leaf::Function;
        root->function = QStringLiteral("wrapper");
        root->series = zones;
        break;
    case 6:
        root->type = Leaf::Function;
        root->function = QStringLiteral("wrapper");
        root->fparms << zones;
        break;
    case 7:
        root->type = Leaf::Compound;
        root->lvalue.b = new QList<Leaf *>{zones};
        break;
    case 8:
        root->type = Leaf::Conditional;
        root->lvalue.l = zones;
        break;
    case 9:
        root->type = Leaf::Conditional;
        root->rvalue.l = zones;
        break;
    case 10:
        root->type = Leaf::Conditional;
        root->cond.l = zones;
        break;
    case 11:
        root->type = Leaf::Index;
        root->lvalue.l = zones;
        break;
    case 12:
        root->type = Leaf::Index;
        root->fparms << zones;
        break;
    case 13:
        root->type = Leaf::Select;
        root->lvalue.l = zones;
        break;
    case 14:
        root->type = Leaf::Select;
        root->fparms << zones;
        break;
    default:
        QFAIL("Unknown AST shape");
    }

    QCOMPARE(DataFilterZones::validateTree(root.get()), 1);
    QVERIFY(zones->inerror);
    QCOMPARE(DataFilterZones::validateTree(root.get()), 0);
}

void TestDataFilterZones::evaluationArgumentsCanonicalizeWithoutValidation()
{
    int status = -1;
    LeafPtr root = parseFormula(QStringLiteral("zones(PaCe,UnItS)"), &status);
    QCOMPARE(status, 0);
    QVERIFY(root);

    const DataFilterSafety::ZoneArguments arguments =
        DataFilterZones::arguments(root.get());

    QVERIFY(arguments.valid);
    QCOMPARE(arguments.series, QStringLiteral("pace"));
    QCOMPARE(arguments.field, QStringLiteral("units"));
    QCOMPARE(*root->fparms.at(0)->lvalue.n, QStringLiteral("PaCe"));
    QCOMPARE(*root->fparms.at(1)->lvalue.n, QStringLiteral("UnItS"));
}

void TestDataFilterZones::functionDefinitionParses()
{
    int status = -1;
    LeafPtr root = parseFormula(QStringLiteral("{ helper { 1; } }"), &status);
    QCOMPARE(status, 0);
    QVERIFY(root);
    QCOMPARE(root->type, Leaf::Compound);
    QCOMPARE(root->lvalue.b->size(), 1);
    QCOMPARE(root->lvalue.b->first()->function, QStringLiteral("helper"));
}

void TestDataFilterZones::parserErrorAfterFunctionReductionCleansTree()
{
    int status = -1;
    LeafPtr root = parseFormula(
        QStringLiteral("sum(zones(POWER,NAME)) +"), &status);

    QVERIFY(status != 0);
    QVERIFY(!root);
}

QTEST_APPLESS_MAIN(TestDataFilterZones)

#include "testDataFilterZones.moc"
