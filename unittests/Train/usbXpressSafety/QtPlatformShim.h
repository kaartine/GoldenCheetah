/*
 * Let Qt establish its real Linux platform before restoring the production
 * adapter's WIN32 feature gate for this cross-platform test build.
 */

#ifndef GC_TEST_USBXPRESS_QT_PLATFORM_SHIM_H
#define GC_TEST_USBXPRESS_QT_PLATFORM_SHIM_H

#ifdef WIN32
#undef WIN32
#define GC_TEST_USBXPRESS_RESTORE_WIN32
#endif

#include <QtCore/qglobal.h>

#ifdef GC_TEST_USBXPRESS_RESTORE_WIN32
#define WIN32
#undef GC_TEST_USBXPRESS_RESTORE_WIN32
#endif

#endif
