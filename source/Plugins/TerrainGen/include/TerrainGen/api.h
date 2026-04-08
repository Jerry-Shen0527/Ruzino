#pragma once

#define RUZINO_NAMESPACE_OPEN_SCOPE  namespace Ruzino {
#define RUZINO_NAMESPACE_CLOSE_SCOPE }

#if defined(_MSC_VER)
#define TERRAINGEN_EXPORT   __declspec(dllexport)
#define TERRAINGEN_IMPORT   __declspec(dllimport)
#define TERRAINGEN_NOINLINE __declspec(noinline)
#define TERRAINGEN_INLINE   __forceinline
#else
#define TERRAINGEN_EXPORT __attribute__((visibility("default")))
#define TERRAINGEN_IMPORT
#define TERRAINGEN_NOINLINE __attribute__((noinline))
#define TERRAINGEN_INLINE   __attribute__((always_inline)) inline
#endif

#if BUILD_TERRAINGEN_MODULE
#define TERRAINGEN_API    TERRAINGEN_EXPORT
#define TERRAINGEN_EXTERN extern
#else
#define TERRAINGEN_API TERRAINGEN_IMPORT
#if defined(_MSC_VER)
#define TERRAINGEN_EXTERN
#else
#define TERRAINGEN_EXTERN extern
#endif
#endif
