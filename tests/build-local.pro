# Pass TEST_NAME=preview_probe (or another listed source stem) to qmake.
QT += core network
CONFIG += c++17 console release no_batch
CONFIG -= debug app_bundle
TEMPLATE = app
isEmpty(TEST_NAME): TEST_NAME = preview_probe
TARGET = $$TEST_NAME
PROJECT_ROOT = $$clean_path($$PWD/..)
SOURCES += $$PWD/$${TEST_NAME}.cpp
INCLUDEPATH += $$PROJECT_ROOT/src $$OUT_PWD/generated
DEFINES += NOMINMAX WIN32_LEAN_AND_MEAN
win32-msvc*: QMAKE_CXXFLAGS += /utf-8
contains(TEST_NAME, ^(booking_ui_offline_test|preview_probe)$) {
    QT += gui widgets
    # Refuse unexpected business I/O and suppress constructor DNS in offline UI tests.
    SOURCES += $$PWD/offline_broker_guard.cpp $$PROJECT_ROOT/src/native_service.cpp $$PROJECT_ROOT/src/booking_service.cpp
    CURL_ROOT = $$(CURL_ROOT)
    !isEmpty(CURL_ROOT) {
        INCLUDEPATH += $$CURL_ROOT/include
        LIBS += -L$$CURL_ROOT/lib
    }
    win32:exists($$CURL_ROOT/lib/libcurl.lib) {
        LIBS += $$CURL_ROOT/lib/libcurl.lib
    } else {
        LIBS += -lcurl
    }
}
contains(TEST_NAME, ^(legacy_service_offline_test|legacy_query_offline_test)$) {
    SOURCES += $$PROJECT_ROOT/src/native_service.cpp
}
equals(TEST_NAME, backend_booking_offline_probe) {
    CURL_ROOT = $$(CURL_ROOT)
    !isEmpty(CURL_ROOT): INCLUDEPATH += $$CURL_ROOT/include
    win32:exists($$CURL_ROOT/lib/libcurl.lib) {
        LIBS += $$CURL_ROOT/lib/libcurl.lib
    } else {
        LIBS += -lcurl
    }
    win32: LIBS += -lws2_32
}
DESTDIR = $$OUT_PWD/bin
