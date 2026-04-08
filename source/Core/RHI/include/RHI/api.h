
#pragma once

#ifndef RUZINO_NAMESPACE_OPEN_SCOPE
#define RUZINO_NAMESPACE_OPEN_SCOPE  namespace Ruzino {
#define RUZINO_NAMESPACE_CLOSE_SCOPE }
#endif

#if defined(_MSC_VER)
#define RHI_EXPORT   __declspec(dllexport)
#define RHI_IMPORT   __declspec(dllimport)
#define RHI_NOINLINE __declspec(noinline)
#define RHI_INLINE   __forceinline
#else
#define RHI_EXPORT __attribute__((visibility("default")))
#define RHI_IMPORT
#define RHI_NOINLINE __attribute__((noinline))
#define RHI_INLINE   __attribute__((always_inline)) inline
#endif

#if BUILD_RHI_MODULE
#define RHI_API    RHI_EXPORT
#define RHI_EXTERN extern
#else
#define RHI_API RHI_IMPORT
#if defined(_MSC_VER)
#define RHI_EXTERN
#else
#define RHI_EXTERN extern
#endif
#endif
