#pragma once

#if defined(_WIN32) && !defined(PVD_CORE_STATIC)
#  if defined(PVD_CORE_BUILD)
#    define PVD_CORE_API __declspec(dllexport)
#  else
#    define PVD_CORE_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && !defined(PVD_CORE_STATIC)
#  define PVD_CORE_API __attribute__((visibility("default")))
#else
#  define PVD_CORE_API
#endif
