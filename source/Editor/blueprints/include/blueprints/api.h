
#pragma once

#ifndef RUZINO_NAMESPACE_OPEN_SCOPE
#define RUZINO_NAMESPACE_OPEN_SCOPE  namespace Ruzino {
#define RUZINO_NAMESPACE_CLOSE_SCOPE }
#endif

#if defined(_MSC_VER)
#define BLUEPRINTS_EXPORT   __declspec(dllexport)
#define BLUEPRINTS_IMPORT   __declspec(dllimport)
#define BLUEPRINTS_NOINLINE __declspec(noinline)
#define BLUEPRINTS_INLINE   __forceinline
#else
#define BLUEPRINTS_EXPORT __attribute__((visibility("default")))
#define BLUEPRINTS_IMPORT
#define BLUEPRINTS_NOINLINE __attribute__((noinline))
#define BLUEPRINTS_INLINE   __attribute__((always_inline)) inline
#endif

#if BUILD_BLUEPRINTS_MODULE
#define BLUEPRINTS_API    BLUEPRINTS_EXPORT
#define BLUEPRINTS_EXTERN extern
#else
#define BLUEPRINTS_API BLUEPRINTS_IMPORT
#if defined(_MSC_VER)
#define BLUEPRINTS_EXTERN
#else
#define BLUEPRINTS_EXTERN extern
#endif
#endif
