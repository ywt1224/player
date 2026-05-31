QT += core websockets
QT -= gui

CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = test_NLS_direct
TEMPLATE = app

SOURCES += main.cpp

# FFmpeg        ffmpeg-4.2.1-win32-dev
INCLUDEPATH += $$PWD/ffmpeg-4.2.1-win32-dev/include
LIBS += $$PWD/ffmpeg-4.2.1-win32-dev/lib/avformat.lib \
        $$PWD/ffmpeg-4.2.1-win32-dev/lib/avcodec.lib  \
        $$PWD/ffmpeg-4.2.1-win32-dev/lib/avutil.lib   \
        $$PWD/ffmpeg-4.2.1-win32-dev/lib/swresample.lib

# OpenSSL (for WSS)
INCLUDEPATH += D:/OpenSSL-Win32/include
LIBS += "D:/OpenSSL-Win32/lib/VC/x86/MDd/libcrypto.lib"
