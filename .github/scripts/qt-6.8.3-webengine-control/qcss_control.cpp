/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

// Control for the QCss InvalidRead seen in IntervalSummaryWindow::IntervalSummaryWindow.
// No GoldenCheetah code is linked. The style string is GCColor::css() (Colors.cpp:547,
// non-macOS branch) with fixed colour and size arguments.
//
//   plain  QTextEdit::setHtml("<body></body>") without a style sheet
//   gccss  QTextEdit::setHtml(GCColor::css() + "<body></body>")
//   file   QTextEdit::setHtml(<exact string captured from IntervalSummaryWindow>)
//   layout as file, 64 times, each after a differently sized live padding allocation.
//          Qt's AES qHash (qhash.cpp aeshash128_lt16) reads short keys with an
//          unaligned 16-byte load that runs past the end of the string only when the
//          key lies in the lower half of a page, so whether the parser's lexeme
//          buffers trigger Memcheck depends on heap layout; this sweeps it.

#include <QApplication>
#include <QTextEdit>
#include <QFile>

#include <cstdio>

static QString gcCss()
{
    return QString("<style> "
                   "html { overflow: auto; }"
                   "body { position: absolute; "
                   "       top: %4px; left: %4px; bottom: %4px; right: %4px; padding: 0px; "
                   "       overflow-y: hidden; overflow-x: hidden; color: %3; background-color: %2; }"
                   "body:hover { overflow-y: scroll; }"
                   "h1 { color: %1; background-color: %2; } "
                   "h2 { color: %1; background-color: %2; } "
                   "h3 { color: %1; background-color: %2; } "
                   "h4 { color: %1; background-color: %2; } "
                   "b { color: %1; background-color: %2; } "
                   "#sharp { color: %1; background-color: darkGray; font-weight: bold; } "
                   ".tooltip { position: relative; display: inline-block; } "
                   ".tooltip .tooltiptext { visibility: hidden; background-color: %2; color: %1; text-align: center; padding: %4px 0; border-radius: %5px; position: absolute; z-index: 1; width: %6px; margin-left: -%7px; top: 100%; left: 50%; opacity: 0; transition: opacity 0.3s; } "
                   ".tooltip:hover .tooltiptext { visibility: visible; opacity: 1; } "
                   "::-webkit-scrollbar-thumb { background-color: darkGray; } "
                   "::-webkit-scrollbar-thumb:hover { background-color: lightGray; } "
                   "::-webkit-scrollbar { width: %5px; background-color: %2; } "
                   "</style> ")
        .arg("#ff0000", "#000000", "#ffffff", "5", "6", "200", "100");
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    const QString variant = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    QTextEdit edit;
    edit.setReadOnly(true);
    if (variant == QLatin1String("plain")) {
        edit.setHtml(QStringLiteral("<body></body>"));
    } else if (variant == QLatin1String("file") && argc > 2) {
        // Exact string captured from GoldenCheetah's IntervalSummaryWindow.
        QFile in(QString::fromLocal8Bit(argv[2]));
        if (!in.open(QIODevice::ReadOnly)) return 3;
        edit.setHtml(QString::fromUtf8(in.readAll()));
    } else if (variant == QLatin1String("layout") && argc > 2) {
        QFile in(QString::fromLocal8Bit(argv[2]));
        if (!in.open(QIODevice::ReadOnly)) return 3;
        const QString html = QString::fromUtf8(in.readAll());
        QList<QByteArray> padding;
        for (int i = 0; i < 64; ++i) {
            padding.append(QByteArray(1 + i * 40, 'x'));
            QTextEdit sweep;
            sweep.setHtml(html);
        }
    } else if (variant == QLatin1String("gccss")) {
        edit.setHtml(gcCss() + QStringLiteral("<body></body>"));
    } else {
        std::fprintf(stderr, "usage: %s plain|gccss|file <html>|layout <html>\n", argv[0]);
        return 2;
    }
    std::printf("variant %s done\n", qPrintable(variant));
    return 0;
}
