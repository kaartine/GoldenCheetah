/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

// Pure-Qt control for the Qt 6.8.3 WebEngine Memcheck exception
// (../qt-6.8.3-webengine.supp).
// No GoldenCheetah code is linked; each variant runs in its own Valgrind process.
//
//   baseline     QApplication only, WebEngine never touched
//   default      only the global WebEngine context via QWebEngineProfile::defaultProfile()
//   otr          own off-the-record profile created and destroyed
//   disk-noloop  disk profile exactly as ContextAthleteApplicationService creates it
//                ("Default" + persistent cookies, no path setters), destroyed without an
//                event loop turn (the chartOwnership fixture pattern)
//   disk-paths-noloop  as disk-noloop plus MainWindow's setCachePath/setPersistentStoragePath
//   view-sethtml  MainWindow's startup hack: dummy QWebEngineView, setHtml, delete at once,
//                then run the event loop briefly like the application
//   disk-exec    the same profile created and destroyed inside a running event loop,
//                followed by 500 ms of event processing before quit (the application pattern)
//   gc-like      the application's WebEngine usage in one window: athlete "Default" profile
//                with paths, the dummy-view startup hack, an LTMWindow-style summary view,
//                a WebPageWindow-style page on the athlete profile and a RideMapWindow-style
//                off-the-record profile with request interceptor, restricted page and
//                scripted setHtml; runs the event loop for 3 s, then tears down in the
//                application's order (window and its views first, athlete profile last)
//   gc-like-pe   gc-like with the event loop driven by QCoreApplication::processEvents()
//                from the program, as MainWindow's constructor does
//   gc-like-alive  gc-like with window, pages and profiles still alive at exit
//   a11y-label   QLabel::setText with accessibility forced on (SplashScreen): starts the
//                AT-SPI bridge and its D-Bus thread
//   pixmap-pool  QPixmap::fromImageInPlace of a large image (NewSideBarItem): conversion
//                on Qt's global thread pool, threads alive at exit
//   pixmap-icon  pixmap-pool with a real 640x639 RGBA icon and imageRGB()'s recolouring
//   pixmap-formats  the icon converted from several source formats
//   gc-startup-pe   WebEngine objects created, then repeated processEvents() before exec()
//   conn-alive   the widgets whose connection records the gate reports as possibly lost
//                (QDateEdit, QDoubleSpinBox, QComboBox::insertItem, QMenu::addMenu), still
//                owned by a live top-level window at exit like DateSettingsEdit/ChartBar
//   conn-deleted the same widgets deleted before exit
//   conn-hidden  as conn-alive, but never shown and no event loop (settings widgets that
//                are constructed and never opened)

#include <QApplication>
#include <QComboBox>
#include <QDateEdit>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QPixmap>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWebEngineDownloadRequest>
#include <QWebEngineProfile>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>

#include <cstdio>
#include <memory>

namespace {

// Mirrors ContextSessionServices.cpp:30-37 and MainWindow.cpp:180-181.
struct AthleteService
{
    // ContextSessionServices.cpp creates the profile; MainWindow.cpp later sets paths.
    explicit AthleteService(const QString &storage = QString())
        : profile(std::make_unique<QWebEngineProfile>(QStringLiteral("Default")))
    {
        profile->setPersistentCookiesPolicy(QWebEngineProfile::ForcePersistentCookies);
        if (!storage.isEmpty()) {
            profile->setCachePath(storage);
            profile->setPersistentStoragePath(storage);
        }
    }
    std::unique_ptr<QWebEngineProfile> profile;
};

// RideMapWindow.cpp MapPageRequestInterceptor: only the map document and tiles pass.
class Interceptor : public QWebEngineUrlRequestInterceptor
{
public:
    using QWebEngineUrlRequestInterceptor::QWebEngineUrlRequestInterceptor;
    void interceptRequest(QWebEngineUrlRequestInfo &info) override
    {
        if (info.requestUrl().scheme() != QLatin1String("qrc")) info.block(true);
    }
};

// RideMapWindow.cpp RestrictedMapPage: no JS dialogs, main-frame navigation gate.
class RestrictedPage : public QWebEnginePage
{
public:
    RestrictedPage(QWebEngineProfile *profile, QObject *parent) : QWebEnginePage(profile, parent) {}
protected:
    bool javaScriptConfirm(const QUrl &, const QString &) override { return false; }
    bool acceptNavigationRequest(const QUrl &url, NavigationType, bool isMainFrame) override
    {
        return isMainFrame && url.scheme() == QLatin1String("qrc");
    }
};

const char *summaryHtml =
    "<html><head><style>body{font-family:sans-serif}td{padding:2px}</style></head><body>"
    "<h2>Summary</h2><table><tr><td>Duration</td><td>1:00:00</td></tr>"
    "<tr><td>Distance</td><td>30.0 km</td></tr></table></body></html>";

const char *mapHtml =
    "<html><head><style>#map{width:100%;height:100%}</style></head><body><div id='map'></div>"
    "<script>var d=document.getElementById('map');for(var i=0;i<50;i++){var t=document.createElement('img');"
    "t.src='https://a.tile.openstreetmap.org/1/'+i+'/0.png';d.appendChild(t);}"
    "window.setTimeout(function(){d.dataset.ready='1';},100);</script></body></html>";

// The application's widget kinds with library-internal connections.
QWidget *connectionWidgets(bool show = true)
{
    auto *window = new QWidget();
    auto *layout = new QVBoxLayout(window);
    for (int i = 0; i < 9; ++i) {
        layout->addWidget(new QDateEdit(window));
        layout->addWidget(new QDoubleSpinBox(window));
        auto *combo = new QComboBox(window);
        combo->insertItem(0, QStringLiteral("a"));
        combo->insertItem(1, QStringLiteral("b"));
        layout->addWidget(combo);
        auto *menu = new QMenu(QStringLiteral("m"), window);
        menu->addMenu(QStringLiteral("sub"));
        menu->addAction(QStringLiteral("act"));
    }
    if (show) window->show();
    return window;
}

// The application's WebEngine usage in one window; returns the map view whose page
// must be deleted before its parented profile (~RideMapWindow).
QWebEngineView *gcLikeWindow(QWidget *window, QWebEngineProfile *athlete)
{
    auto *layout = new QVBoxLayout(window);
    auto *dummy = new QWebEngineView();
    dummy->page()->setHtml(QStringLiteral("<html></html>"));
    layout->addWidget(dummy);
    layout->removeWidget(dummy);
    delete dummy;

    auto *summary = new QWebEngineView(window);
    summary->page()->setHtml(QString::fromLatin1(summaryHtml));
    layout->addWidget(summary);

    auto *web = new QWebEngineView(window);
    web->setPage(new QWebEnginePage(athlete, web));
    web->page()->setHtml(QString::fromLatin1(summaryHtml));
    layout->addWidget(web);

    auto *mapProfile = new QWebEngineProfile(window);
    mapProfile->setHttpCacheType(QWebEngineProfile::MemoryHttpCache);
    mapProfile->setHttpCacheMaximumSize(16 * 1024 * 1024);
    mapProfile->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
    mapProfile->setPersistentPermissionsPolicy(
        QWebEngineProfile::PersistentPermissionsPolicy::AskEveryTime);
    mapProfile->setSpellCheckEnabled(false);
    mapProfile->setPushServiceEnabled(false);
    mapProfile->setUrlRequestInterceptor(new Interceptor(mapProfile));
    QObject::connect(mapProfile, &QWebEngineProfile::downloadRequested, mapProfile,
                     [](QWebEngineDownloadRequest *download) { download->cancel(); });
    auto *map = new QWebEngineView(window);
    map->setPage(new RestrictedPage(mapProfile, map));
    map->settings()->setAttribute(QWebEngineSettings::LocalStorageEnabled, false);
    map->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
    map->page()->setHtml(QString::fromLatin1(mapHtml), QUrl(QStringLiteral("qrc:/web/ride-map")));
    layout->addWidget(map);

    window->resize(800, 600);
    window->show();
    return map;
}

QWidget *liveWindow = nullptr;  // reachable at exit, like the application's chart windows
AthleteService *liveService = nullptr;  // reachable at exit

} // namespace

int main(int argc, char **argv)
{
    if (argc > 1 && qstrcmp(argv[1], "a11y-label") == 0)
        qputenv("QT_LINUX_ACCESSIBILITY_ALWAYS_ON", "1");
    QApplication app(argc, argv);
    const QString variant = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    QTemporaryDir storage;
    if (!storage.isValid()) return 3;

    if (variant == QLatin1String("baseline")) {
    } else if (variant == QLatin1String("default")) {
        std::printf("default profile off-the-record: %d\n",
                    QWebEngineProfile::defaultProfile()->isOffTheRecord());
    } else if (variant == QLatin1String("otr")) {
        auto profile = std::make_unique<QWebEngineProfile>();
        profile.reset();
    } else if (variant == QLatin1String("disk-noloop")) {
        auto service = std::make_unique<AthleteService>();
        service.reset();
    } else if (variant == QLatin1String("disk-paths-noloop")) {
        auto service = std::make_unique<AthleteService>(storage.path());
        service.reset();
    } else if (variant == QLatin1String("view-sethtml")) {
        QTimer::singleShot(0, &app, [&]() {
            auto *view = new QWebEngineView();
            view->page()->setHtml(QStringLiteral("<html></html>"));
            delete view;
            QTimer::singleShot(500, &app, &QCoreApplication::quit);
        });
        app.exec();
    } else if (variant == QLatin1String("disk-exec")) {
        QTimer::singleShot(0, &app, [&]() {
            auto service = std::make_unique<AthleteService>(storage.path());
            service.reset();
            QTimer::singleShot(500, &app, &QCoreApplication::quit);
        });
        app.exec();
    } else if (variant == QLatin1String("gc-like")) {
        auto service = std::make_unique<AthleteService>(storage.path());
        auto *window = new QWidget();
        QWebEngineView *map = nullptr;
        QTimer::singleShot(0, &app, [&]() {
            map = gcLikeWindow(window, service->profile.get());
            QTimer::singleShot(3000, &app, &QCoreApplication::quit);
        });
        app.exec();
        delete map->page();  // ~RideMapWindow: page before its parented profile
        delete window;
        service.reset();
    } else if (variant == QLatin1String("gc-like-pe")) {
        // MainWindow's constructor drives the event loop with processEvents().
        auto service = std::make_unique<AthleteService>(storage.path());
        auto *window = new QWidget();
        QWebEngineView *map = gcLikeWindow(window, service->profile.get());
        QElapsedTimer elapsed;
        elapsed.start();
        while (elapsed.elapsed() < 3000) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        delete map->page();
        delete window;
        service.reset();
    } else if (variant == QLatin1String("gc-like-alive")) {
        // As gc-like, but the window, its pages and both profiles are still alive
        // at exit (reachable from a global), as when the application quits.
        liveService = new AthleteService(storage.path());
        liveWindow = new QWidget();
        QTimer::singleShot(0, &app, [&]() {
            gcLikeWindow(liveWindow, liveService->profile.get());
            QTimer::singleShot(3000, &app, &QCoreApplication::quit);
        });
        app.exec();
    } else if (variant == QLatin1String("a11y-label")) {
        // SplashScreen: the first QLabel::setText with accessibility on starts the
        // AT-SPI bridge and its D-Bus connection thread.
        liveWindow = new QLabel();
        static_cast<QLabel *>(liveWindow)->setText(QStringLiteral("Loading"));
        liveWindow->show();
        QTimer::singleShot(500, &app, &QCoreApplication::quit);
        app.exec();
    } else if (variant == QLatin1String("pixmap-pool")) {
        // NewSideBarItem: QPixmap::fromImageInPlace converts large images on Qt's
        // global thread pool, whose threads are still alive at exit.
        QImage image(1024, 1024, QImage::Format_ARGB32);
        image.fill(Qt::red);
        const QPixmap pixmap = QPixmap::fromImage(std::move(image),
                                                  Qt::ColorOnly | Qt::PreferDither | Qt::DiffuseAlphaDither);
        std::printf("pixmap %dx%d\n", pixmap.width(), pixmap.height());
    } else if (variant == QLatin1String("pixmap-icon") && argc > 2) {
        // NewSideBarItem::configChanged with the application's own icon file as input
        // data: recolour black pixels like imageRGB(), then convert with its flags.
        QImage icon(QString::fromLocal8Bit(argv[2]));
        if (icon.isNull()) return 3;
        for (int x = 0; x < icon.width(); ++x)
            for (int y = 0; y < icon.height(); ++y)
                if (icon.pixelColor(x, y).rgb() == qRgb(0, 0, 0)) icon.setPixelColor(x, y, QColor(255, 255, 254));
        const QPixmap pixmap = QPixmap::fromImage(std::move(icon),
                                                  Qt::ColorOnly | Qt::PreferDither | Qt::DiffuseAlphaDither);
        std::printf("pixmap %dx%d\n", pixmap.width(), pixmap.height());
    } else if (variant == QLatin1String("pixmap-formats") && argc > 2) {
        // pixmap-icon from several source formats: the conversion (and Qt's GUI
        // thread pool) only runs when the source differs from the pixmap's format.
        const QImage icon(QString::fromLocal8Bit(argv[2]));
        if (icon.isNull()) return 3;
        std::printf("icon format %d\n", int(icon.format()));
        for (QImage::Format format : {icon.format(), QImage::Format_ARGB32, QImage::Format_RGBA8888,
                                      QImage::Format_RGB888, QImage::Format_Indexed8,
                                      QImage::Format_RGBA64}) {
            QImage copy = icon.convertToFormat(format);
            const QPixmap pixmap = QPixmap::fromImage(std::move(copy),
                                                      Qt::ColorOnly | Qt::PreferDither | Qt::DiffuseAlphaDither);
            std::printf("format %d -> pixmap depth %d\n", int(format), pixmap.depth());
        }
    } else if (variant == QLatin1String("gc-startup-pe")) {
        // MainWindow's constructor: WebEngine objects are created, then
        // processEvents() runs repeatedly before QApplication::exec().
        auto service = std::make_unique<AthleteService>(storage.path());
        auto *window = new QWidget();
        QWebEngineView *map = gcLikeWindow(window, service->profile.get());
        for (int i = 0; i < 40; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(25);
        }
        QTimer::singleShot(1000, &app, &QCoreApplication::quit);
        app.exec();
        delete map->page();
        delete window;
        service.reset();
    } else if (variant == QLatin1String("conn-alive")) {
        liveWindow = connectionWidgets();
        QTimer::singleShot(500, &app, &QCoreApplication::quit);
        app.exec();
    } else if (variant == QLatin1String("conn-hidden")) {
        liveWindow = connectionWidgets(false);
    } else if (variant == QLatin1String("conn-deleted")) {
        QWidget *window = connectionWidgets();
        QTimer::singleShot(500, &app, &QCoreApplication::quit);
        app.exec();
        delete window;
    } else {
        std::fprintf(stderr, "usage: %s baseline|default|otr|disk-noloop|disk-paths-noloop|view-sethtml|disk-exec|gc-like|gc-like-pe|gc-like-alive|a11y-label|pixmap-pool|pixmap-icon <png>|pixmap-formats <png>|gc-startup-pe|conn-alive|conn-hidden|conn-deleted\n", argv[0]);
        return 2;
    }
    std::printf("variant %s done\n", qPrintable(variant));
    return 0;
}
