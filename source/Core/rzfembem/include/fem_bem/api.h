
#pragma once

#define RUZINO_NAMESPACE_OPEN_SCOPE  namespace Ruzino {
#define RUZINO_NAMESPACE_CLOSE_SCOPE }

#if defined(_MSC_VER)
#define RZFEMBEM_EXPORT   __declspec(dllexport)
#define RZFEMBEM_IMPORT   __declspec(dllimport)
#define RZFEMBEM_NOINLINE __declspec(noinline)
#define RZFEMBEM_INLINE   __forceinline
#else
#define RZFEMBEM_EXPORT __attribute__((visibility("default")))
#define RZFEMBEM_IMPORT
#define RZFEMBEM_NOINLINE __attribute__((noinline))
#define RZFEMBEM_INLINE   __attribute__((always_inline)) inline
#endif

#if BUILD_RZFEMBEM_MODULE
#define RZFEMBEM_API    RZFEMBEM_EXPORT
#define RZFEMBEM_EXTERN extern
#else
#define RZFEMBEM_API RZFEMBEM_IMPORT
#if defined(_MSC_VER)
#define RZFEMBEM_EXTERN
#else
#define RZFEMBEM_EXTERN extern
#endif
#endif
