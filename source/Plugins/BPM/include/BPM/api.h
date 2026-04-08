
#pragma once

#define RUZINO_NAMESPACE_OPEN_SCOPE  namespace Ruzino {
#define RUZINO_NAMESPACE_CLOSE_SCOPE }

#if defined(_MSC_VER)
#define BPM_EXPORT   __declspec(dllexport)
#define BPM_IMPORT   __declspec(dllimport)
#define BPM_NOINLINE __declspec(noinline)
#define BPM_INLINE   __forceinline
#else
#define BPM_EXPORT __attribute__((visibility("default")))
#define BPM_IMPORT
#define BPM_NOINLINE __attribute__((noinline))
#define BPM_INLINE   __attribute__((always_inline)) inline
#endif

#if BUILD_BPM_MODULE
#define BPM_API    BPM_EXPORT
#define BPM_EXTERN extern
#else
#define BPM_API BPM_IMPORT
#if defined(_MSC_VER)
#define BPM_EXTERN
#else
#define BPM_EXTERN extern
#endif
#endif
