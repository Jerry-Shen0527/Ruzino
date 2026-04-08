
#pragma once

#define RUZINO_NAMESPACE_OPEN_SCOPE  namespace Ruzino {
#define RUZINO_NAMESPACE_CLOSE_SCOPE }

#if defined(_MSC_VER)
#define RZSIM_EXPORT   __declspec(dllexport)
#define RZSIM_IMPORT   __declspec(dllimport)
#define RZSIM_NOINLINE __declspec(noinline)
#define RZSIM_INLINE   __forceinline
#else
#define RZSIM_EXPORT __attribute__((visibility("default")))
#define RZSIM_IMPORT
#define RZSIM_NOINLINE __attribute__((noinline))
#define RZSIM_INLINE   __attribute__((always_inline)) inline
#endif

#if BUILD_RZSIM_MODULE
#define RZSIM_API    RZSIM_EXPORT
#define RZSIM_EXTERN extern
#else
#define RZSIM_API RZSIM_IMPORT
#if defined(_MSC_VER)
#define RZSIM_EXTERN
#else
#define RZSIM_EXTERN extern
#endif
#endif
