#pragma once

#ifndef RUZINO_NAMESPACE_OPEN_SCOPE
#define RUZINO_NAMESPACE_OPEN_SCOPE  namespace Ruzino {
#define RUZINO_NAMESPACE_CLOSE_SCOPE }
#endif

#if defined(_MSC_VER)
#define INPUT_EXPORT   __declspec(dllexport)
#define INPUT_IMPORT   __declspec(dllimport)
#define INPUT_NOINLINE __declspec(noinline)
#define INPUT_INLINE   __forceinline
#else
#define INPUT_EXPORT __attribute__((visibility("default")))
#define INPUT_IMPORT
#define INPUT_NOINLINE __attribute__((noinline))
#define INPUT_INLINE   __attribute__((always_inline)) inline
#endif

#if BUILD_INPUT_MODULE
#define INPUT_API    INPUT_EXPORT
#define INPUT_EXTERN extern
#else
#define INPUT_API INPUT_IMPORT
#if defined(_MSC_VER)
#define INPUT_EXTERN
#else
#define INPUT_EXTERN extern
#endif
#endif
