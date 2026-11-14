#pragma once
#if defined(_WIN32) && !defined(PVD_ANALYSIS_STATIC)
#  if defined(PVD_ANALYSIS_BUILD)
#    define PVD_ANALYSIS_API __declspec(dllexport)
#  else
#    define PVD_ANALYSIS_API __declspec(dllimport)
#  endif
#else
#  define PVD_ANALYSIS_API
#endif
