# Independent tests for the new local session store and incremental refresh.
# Generate OUT_PWD/generated/main.moc with Qt moc before building the GUI test.
QT += core network
CONFIG += c++17 console release no_batch
CONFIG -= debug app_bundle
TEMPLATE = app
isEmpty(TEST_NAME): TEST_NAME = session_store_offline_test
TARGET = $$TEST_NAME
PROJECT_ROOT = $$clean_path($$PWD/..)
SOURCES += $$PWD/$$eval(TEST_NAME).cpp
INCLUDEPATH += $$PROJECT_ROOT/src $$OUT_PWD/generated
DEFINES += NOMINMAX WIN32_LEAN_AND_MEAN
win32-msvc*: QMAKE_CXXFLAGS += /utf-8
equals(TEST_NAME, smooth_refresh_offline_test) {
    QT += gui widgets
    SOURCES += $$PWD/offline_broker_guard.cpp $$PROJECT_ROOT/src/native_service.cpp $$PROJECT_ROOT/src/booking_service.cpp
    DEFINES += SMOOTH_CANDIDATE
}
DESTDIR = $$OUT_PWD/bin
