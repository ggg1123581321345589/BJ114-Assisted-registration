# Usage: qmake ../../tests/latest-offline.pro TEST_NAME=pool_recovery_offline_test
QT += core network
CONFIG += c++17 console release no_batch
CONFIG -= debug app_bundle
TEMPLATE = app
isEmpty(TEST_NAME): TEST_NAME = pool_recovery_offline_test
TARGET = $$TEST_NAME
PROJECT_ROOT = $$clean_path($$PWD/..)
SOURCES += $$PWD/$$eval(TEST_NAME).cpp
INCLUDEPATH += $$PROJECT_ROOT/src $$OUT_PWD/generated
DEFINES += NOMINMAX WIN32_LEAN_AND_MEAN
win32-msvc*: QMAKE_CXXFLAGS += /utf-8
contains(TEST_NAME, ^(pool_recovery_offline_test|parallel_query_offline_test)$) {
    QT += gui widgets
    SOURCES += $$PWD/offline_broker_guard.cpp $$PROJECT_ROOT/src/native_service.cpp $$PROJECT_ROOT/src/booking_service.cpp
    DEFINES += HAS_QUERY_POOL HAS_DYNAMIC_POOL
}
DESTDIR = $$OUT_PWD/bin
