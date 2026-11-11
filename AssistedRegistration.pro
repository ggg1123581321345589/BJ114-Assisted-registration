QT += core gui widgets network
CONFIG += c++17 release
CONFIG -= debug console
TEMPLATE = app
TARGET = AssistedRegistration
SOURCES += src/main.cpp src/native_backend.cpp src/native_service.cpp
HEADERS += src/native_backend.h src/native_service.h
DEFINES += NOMINMAX WIN32_LEAN_AND_MEAN
CURL_ROOT = $$(CURL_ROOT)
!isEmpty(CURL_ROOT) {
    INCLUDEPATH += "$$CURL_ROOT/include"
    LIBS += -L"$$CURL_ROOT/lib"
}
win32:exists("$$CURL_ROOT/lib/libcurl.lib") {
    LIBS += "$$CURL_ROOT/lib/libcurl.lib"
} else {
    LIBS += -lcurl
}
win32-msvc*: QMAKE_CXXFLAGS += /utf-8
DESTDIR = $$PWD/bin
