
#pragma once

#ifndef RUZINO_NAMESPACE_OPEN_SCOPE
#define RUZINO_NAMESPACE_OPEN_SCOPE  namespace Ruzino {
#define RUZINO_NAMESPACE_CLOSE_SCOPE }
#endif

#if defined(_MSC_VER)
#define GPUCONTEXT_EXPORT   __declspec(dllexport)
#define GPUCONTEXT_IMPORT   __declspec(dllimport)
#define GPUCONTEXT_NOINLINE __declspec(noinline)
#define GPUCONTEXT_INLINE   __forceinline
#else
#define GPUCONTEXT_EXPORT __attribute__((visibility("default")))
#define GPUCONTEXT_IMPORT
#define GPUCONTEXT_NOINLINE __attribute__((noinline))
#define GPUCONTEXT_INLINE   __attribute__((always_inline)) inline
#endif

#if BUILD_GPUCONTEXT_MODULE
#define GPUCONTEXT_API    GPUCONTEXT_EXPORT
#define GPUCONTEXT_EXTERN extern
#else
#define GPUCONTEXT_API GPUCONTEXT_IMPORT
#if defined(_MSC_VER)
#define GPUCONTEXT_EXTERN
#else
#define GPUCONTEXT_EXTERN extern
#endif
#endif
