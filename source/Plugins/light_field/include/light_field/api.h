
#pragma once

#define RUZINO_NAMESPACE_OPEN_SCOPE  namespace Ruzino {
#define RUZINO_NAMESPACE_CLOSE_SCOPE }

#if defined(_MSC_VER)
#define LIGHT_FIELD_EXPORT   __declspec(dllexport)
#define LIGHT_FIELD_IMPORT   __declspec(dllimport)
#define LIGHT_FIELD_NOINLINE __declspec(noinline)
#define LIGHT_FIELD_INLINE   __forceinline
#else
#define LIGHT_FIELD_EXPORT __attribute__((visibility("default")))
#define LIGHT_FIELD_IMPORT
#define LIGHT_FIELD_NOINLINE __attribute__((noinline))
#define LIGHT_FIELD_INLINE   __attribute__((always_inline)) inline
#endif

#if BUILD_LIGHT_FIELD_MODULE
#define LIGHT_FIELD_API    LIGHT_FIELD_EXPORT
#define LIGHT_FIELD_EXTERN extern
#else
#define LIGHT_FIELD_API LIGHT_FIELD_IMPORT
#if defined(_MSC_VER)
#define LIGHT_FIELD_EXTERN
#else
#define LIGHT_FIELD_EXTERN extern
#endif
#endif
