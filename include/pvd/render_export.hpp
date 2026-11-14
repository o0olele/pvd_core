#pragma once

#if defined(_WIN32) && !defined(PVD_RENDER_STATIC)
#  if defined(PVD_RENDER_BUILD)
#    define PVD_RENDER_API __declspec(dllexport)
#  else
#    define PVD_RENDER_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && !defined(PVD_RENDER_STATIC)
#  define PVD_RENDER_API __attribute__((visibility("default")))
#else
#  define PVD_RENDER_API
#endif
