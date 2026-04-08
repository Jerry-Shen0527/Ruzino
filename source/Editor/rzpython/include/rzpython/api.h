
#pragma once

#ifndef RUZINO_NAMESPACE_OPEN_SCOPE
#define RUZINO_NAMESPACE_OPEN_SCOPE  namespace Ruzino {
#define RUZINO_NAMESPACE_CLOSE_SCOPE }
#endif

#if defined(_MSC_VER)
#define RZPYTHON_EXPORT   __declspec(dllexport)
#define RZPYTHON_IMPORT   __declspec(dllimport)
#define RZPYTHON_NOINLINE __declspec(noinline)
#define RZPYTHON_INLINE   __forceinline
#else
#define RZPYTHON_EXPORT __attribute__((visibility("default")))
#define RZPYTHON_IMPORT
#define RZPYTHON_NOINLINE __attribute__((noinline))
#define RZPYTHON_INLINE   __attribute__((always_inline)) inline
#endif

#if BUILD_RZPYTHON_MODULE
#define RZPYTHON_API    RZPYTHON_EXPORT
#define RZPYTHON_EXTERN extern
#else
#define RZPYTHON_API RZPYTHON_IMPORT
#if defined(_MSC_VER)
#define RZPYTHON_EXTERN
#else
#define RZPYTHON_EXTERN extern
#endif
#endif
