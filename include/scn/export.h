#pragma once

#if defined(_WIN32) || defined(__CYGWIN__)
#define SCN_EXPORT_HEADER_EXPORT __declspec(dllexport)
#define SCN_EXPORT_HEADER_IMPORT __declspec(dllimport)
#else
#define SCN_EXPORT_HEADER_EXPORT __attribute__((visibility("default")))
#define SCN_EXPORT_HEADER_IMPORT __attribute__((visibility("default")))
#endif

#define SCN_EXPORT_HEADER_NO_EXPORT
