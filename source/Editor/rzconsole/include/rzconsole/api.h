
#pragma once

#ifndef RUZINO_NAMESPACE_OPEN_SCOPE
#define RUZINO_NAMESPACE_OPEN_SCOPE  namespace Ruzino {
#define RUZINO_NAMESPACE_CLOSE_SCOPE }
#endif

#if defined(_MSC_VER)
#define RZCONSOLE_EXPORT   __declspec(dllexport)
#define RZCONSOLE_IMPORT   __declspec(dllimport)
#define RZCONSOLE_NOINLINE __declspec(noinline)
#define RZCONSOLE_INLINE   __forceinline
#else
#define RZCONSOLE_EXPORT __attribute__((visibility("default")))
#define RZCONSOLE_IMPORT
#define RZCONSOLE_NOINLINE __attribute__((noinline))
#define RZCONSOLE_INLINE   __attribute__((always_inline)) inline
#endif

#if BUILD_RZCONSOLE_MODULE
#define RZCONSOLE_API    RZCONSOLE_EXPORT
#define RZCONSOLE_EXTERN extern
#else
#define RZCONSOLE_API RZCONSOLE_IMPORT
#if defined(_MSC_VER)
#define RZCONSOLE_EXTERN
#else
#define RZCONSOLE_EXTERN extern
#endif
#endif
