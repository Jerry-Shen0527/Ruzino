#pragma once

#ifdef _WIN32
#ifdef BUILD_RZSIM_CUDA_MODULE
#define RZSIM_CUDA_API __declspec(dllexport)
#else
#define RZSIM_CUDA_API __declspec(dllimport)
#endif
#else
#define RZSIM_CUDA_API
#endif
