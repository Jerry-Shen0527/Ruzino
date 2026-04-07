
#pragma once

#define RUZINO_NAMESPACE_OPEN_SCOPE  namespace Ruzino {
#define RUZINO_NAMESPACE_CLOSE_SCOPE }

#if defined(_MSC_VER)
#define TREEGEN_EXPORT   __declspec(dllexport)
#define TREEGEN_IMPORT   __declspec(dllimport)
#define TREEGEN_NOINLINE __declspec(noinline)
#define TREEGEN_INLINE   __forceinline
#else
#define TREEGEN_EXPORT __attribute__((visibility("default")))
#define TREEGEN_IMPORT
#define TREEGEN_NOINLINE __attribute__((noinline))
#define TREEGEN_INLINE   __attribute__((always_inline)) inline
#endif

#if BUILD_TREEGEN_MODULE
#define TREEGEN_API    TREEGEN_EXPORT
#define TREEGEN_EXTERN extern
#else
#define TREEGEN_API TREEGEN_IMPORT
#if defined(_MSC_VER)
#define TREEGEN_EXTERN
#else
#define TREEGEN_EXTERN extern
#endif
#endif
