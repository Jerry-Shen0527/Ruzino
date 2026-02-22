
#pragma once

#ifndef RUZINO_NAMESPACE_OPEN_SCOPE
#define RUZINO_NAMESPACE_OPEN_SCOPE namespace Ruzino {
#endif
#ifndef RUZINO_NAMESPACE_CLOSE_SCOPE
#define RUZINO_NAMESPACE_CLOSE_SCOPE }
#endif

#if defined(_MSC_VER)
#define HD_RUZINO_GL_EXPORT   __declspec(dllexport)
#define HD_RUZINO_GL_IMPORT   __declspec(dllimport)
#define HD_RUZINO_GL_NOINLINE __declspec(noinline)
#define HD_RUZINO_GL_INLINE   __forceinline
#else
#define HD_RUZINO_GL_EXPORT __attribute__((visibility("default")))
#define HD_RUZINO_GL_IMPORT
#define HD_RUZINO_GL_NOINLINE __attribute__((noinline))
#define HD_RUZINO_GL_INLINE   __attribute__((always_inline)) inline
#endif

#if BUILD_HD_RUZINO_GL_MODULE
#define HD_RUZINO_GL_API    HD_RUZINO_GL_EXPORT
#define HD_RUZINO_GL_EXTERN extern
#else
#define HD_RUZINO_GL_API HD_RUZINO_GL_IMPORT
#if defined(_MSC_VER)
#define HD_RUZINO_GL_EXTERN
#else
#define HD_RUZINO_GL_EXTERN extern
#endif
#endif
