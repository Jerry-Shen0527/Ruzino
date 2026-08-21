#pragma once

#ifndef RUZINO_NAMESPACE_OPEN_SCOPE
#define RUZINO_NAMESPACE_OPEN_SCOPE  namespace Ruzino {
#define RUZINO_NAMESPACE_CLOSE_SCOPE }
#endif

#if defined(_MSC_VER)
#define CHARACTER_EXPORT   __declspec(dllexport)
#define CHARACTER_IMPORT   __declspec(dllimport)
#define CHARACTER_NOINLINE __declspec(noinline)
#define CHARACTER_INLINE   __forceinline
#else
#define CHARACTER_EXPORT __attribute__((visibility("default")))
#define CHARACTER_IMPORT
#define CHARACTER_NOINLINE __attribute__((noinline))
#define CHARACTER_INLINE   __attribute__((always_inline)) inline
#endif

#if BUILD_CHARACTER_MODULE
#define CHARACTER_API    CHARACTER_EXPORT
#define CHARACTER_EXTERN extern
#else
#define CHARACTER_API CHARACTER_IMPORT
#if defined(_MSC_VER)
#define CHARACTER_EXTERN
#else
#define CHARACTER_EXTERN extern
#endif
#endif
