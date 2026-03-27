
#ifndef OVM_EXPORT_H
#define OVM_EXPORT_H

#ifdef OVM_STATIC_DEFINE
#  define OVM_EXPORT
#  define OVM_NO_EXPORT
#else
#  ifndef OVM_EXPORT
#    ifdef OpenVolumeMesh_EXPORTS
        /* We are building this library */
#      define OVM_EXPORT __attribute__((visibility("default")))
#    else
        /* We are using this library */
#      define OVM_EXPORT __attribute__((visibility("default")))
#    endif
#  endif

#  ifndef OVM_NO_EXPORT
#    define OVM_NO_EXPORT __attribute__((visibility("hidden")))
#  endif
#endif

#ifndef CMAKE_OVM_DEPRECATED
#  define CMAKE_OVM_DEPRECATED __attribute__ ((__deprecated__))
#endif

#ifndef CMAKE_OVM_DEPRECATED_EXPORT
#  define CMAKE_OVM_DEPRECATED_EXPORT OVM_EXPORT CMAKE_OVM_DEPRECATED
#endif

#ifndef CMAKE_OVM_DEPRECATED_NO_EXPORT
#  define CMAKE_OVM_DEPRECATED_NO_EXPORT OVM_NO_EXPORT CMAKE_OVM_DEPRECATED
#endif

#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef CMAKE_OVM_NO_DEPRECATED
#    define CMAKE_OVM_NO_DEPRECATED
#  endif
#endif

#endif /* OVM_EXPORT_H */
