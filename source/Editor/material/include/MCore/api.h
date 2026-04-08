
#pragma once

#ifndef RUZINO_NAMESPACE_OPEN_SCOPE
#define RUZINO_NAMESPACE_OPEN_SCOPE  namespace Ruzino {
#define RUZINO_NAMESPACE_CLOSE_SCOPE }
#endif

#if defined(_MSC_VER)
#define MCORE_EXPORT   __declspec(dllexport)
#define MCORE_IMPORT   __declspec(dllimport)
#define MCORE_NOINLINE __declspec(noinline)
#define MCORE_INLINE   __forceinline
#else
#define MCORE_EXPORT __attribute__((visibility("default")))
#define MCORE_IMPORT
#define MCORE_NOINLINE __attribute__((noinline))
#define MCORE_INLINE   __attribute__((always_inline)) inline
#endif

#if BUILD_MCORE_MODULE
#define MCORE_API    MCORE_EXPORT
#define MCORE_EXTERN extern
#else
#define MCORE_API MCORE_IMPORT
#if defined(_MSC_VER)
#define MCORE_EXTERN
#else
#define MCORE_EXTERN extern
#endif
#endif
