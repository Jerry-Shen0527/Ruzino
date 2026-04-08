
#pragma once

#define RUZINO_NAMESPACE_OPEN_SCOPE  namespace Ruzino {
#define RUZINO_NAMESPACE_CLOSE_SCOPE }

#if defined(_MSC_VER)
#define RZSOLVER_EXPORT   __declspec(dllexport)
#define RZSOLVER_IMPORT   __declspec(dllimport)
#define RZSOLVER_NOINLINE __declspec(noinline)
#define RZSOLVER_INLINE   __forceinline
#else
#define RZSOLVER_EXPORT __attribute__((visibility("default")))
#define RZSOLVER_IMPORT
#define RZSOLVER_NOINLINE __attribute__((noinline))
#define RZSOLVER_INLINE   __attribute__((always_inline)) inline
#endif

#if BUILD_RZSOLVER_MODULE
#define RZSOLVER_API    RZSOLVER_EXPORT
#define RZSOLVER_EXTERN extern
#else
#define RZSOLVER_API RZSOLVER_IMPORT
#if defined(_MSC_VER)
#define RZSOLVER_EXTERN
#else
#define RZSOLVER_EXTERN extern
#endif
#endif
