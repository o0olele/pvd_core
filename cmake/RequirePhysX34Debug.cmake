if(NOT SELECTED_CONFIG STREQUAL "Debug")
    message(FATAL_ERROR "PhysX 3.4 capture uses the supplied Debug SDK; build with --config Debug")
endif()
