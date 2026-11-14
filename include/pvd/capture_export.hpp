#pragma once

#if defined(_WIN32) && !defined(PVD_TRANSPORT_STATIC)
#  if defined(PVD_TRANSPORT_BUILD)
#    define PVD_TRANSPORT_API __declspec(dllexport)
#  else
#    define PVD_TRANSPORT_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && !defined(PVD_TRANSPORT_STATIC)
#  define PVD_TRANSPORT_API __attribute__((visibility("default")))
#else
#  define PVD_TRANSPORT_API
#endif
