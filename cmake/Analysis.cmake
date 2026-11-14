# Analysis is independent of transport, rendering, ImGui, Windows and the SDK.
add_library(pvd_analysis src/profile.cpp src/memory.cpp src/errors.cpp src/curves.cpp
    src/memory_bootstrap.cpp src/profile_bootstrap.cpp src/profile_curves.cpp)
add_library(pvd::analysis ALIAS pvd_analysis)
set_target_properties(pvd_analysis PROPERTIES EXPORT_NAME analysis VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR} CXX_EXTENSIONS OFF)
target_compile_features(pvd_analysis PUBLIC cxx_std_17)
target_link_libraries(pvd_analysis PUBLIC pvd::core)
target_compile_definitions(pvd_analysis PRIVATE PVD_ANALYSIS_BUILD)
if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(pvd_analysis PUBLIC PVD_ANALYSIS_STATIC)
endif()
if(MSVC)
    target_compile_options(pvd_analysis PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(pvd_analysis PRIVATE -Wall -Wextra -Wpedantic)
endif()
list(APPEND PVD_INSTALL_TARGETS pvd_analysis)

if(BUILD_TESTING)
    foreach(kind IN ITEMS profile memory error curves memory_bootstrap profile_bootstrap profile_curves)
        add_executable(pvd_${kind}_tests tests/${kind}_tests.cpp)
        target_link_libraries(pvd_${kind}_tests PRIVATE pvd::analysis)
        add_test(NAME ${kind} COMMAND pvd_${kind}_tests)
        set_tests_properties(${kind} PROPERTIES TIMEOUT 30)
    endforeach()
endif()

if(PVD_BUILD_PHYSX34_VALIDATION)
    include(${CMAKE_CURRENT_LIST_DIR}/PhysX34.cmake)
    pvd_add_physx34_capture(pvd_physx34_profile_capture
        ${PROJECT_SOURCE_DIR}/examples/physx34_profile_capture.cpp)
    target_include_directories(pvd_physx34_profile_capture SYSTEM PRIVATE
        ${PROJECT_SOURCE_DIR}/examples/physx34_compat
        ${PHYSX34_ROOT}/PxShared/src/foundation/include
        ${PHYSX34_ROOT}/PxShared/src/pvd/src)
    pvd_add_physx34_capture(pvd_physx34_profile_frames_capture
        ${PROJECT_SOURCE_DIR}/examples/physx34_profile_frames_capture.cpp)
    target_include_directories(pvd_physx34_profile_frames_capture SYSTEM PRIVATE
        ${PROJECT_SOURCE_DIR}/examples/physx34_compat
        ${PHYSX34_ROOT}/PxShared/src/foundation/include
        ${PHYSX34_ROOT}/PxShared/src/pvd/src)
    pvd_add_physx34_capture(pvd_physx34_memory_capture
        ${PROJECT_SOURCE_DIR}/examples/physx34_memory_capture.cpp)
    target_include_directories(pvd_physx34_memory_capture SYSTEM PRIVATE
        ${PROJECT_SOURCE_DIR}/examples/physx34_compat
        ${PHYSX34_ROOT}/PxShared/src/foundation/include)
    if(BUILD_TESTING)
        file(MAKE_DIRECTORY ${PROJECT_BINARY_DIR}/fixtures)
        add_test(NAME physx34_profile_frames_generate COMMAND pvd_physx34_profile_frames_capture
            ${PROJECT_BINARY_DIR}/fixtures/profile_frames)
        set_tests_properties(physx34_profile_frames_generate PROPERTIES FIXTURES_SETUP physx34_profile_frames TIMEOUT 30)
        add_test(NAME physx34_profile_curves COMMAND pvd_profile_curves_tests
            ${PROJECT_BINARY_DIR}/fixtures/profile_frames_capture.pxd2
            ${PROJECT_BINARY_DIR}/fixtures/profile_frames_expected.csv
            ${PROJECT_BINARY_DIR}/fixtures/profile_frames_windows.csv
            ${PROJECT_BINARY_DIR}/fixtures/profile_frames_scopes.csv)
        set_tests_properties(physx34_profile_curves PROPERTIES FIXTURES_REQUIRED physx34_profile_frames TIMEOUT 30)
        add_test(NAME physx34_profile_generate COMMAND pvd_physx34_profile_capture
            ${PROJECT_BINARY_DIR}/fixtures/profile)
        set_tests_properties(physx34_profile_generate PROPERTIES FIXTURES_SETUP physx34_profile TIMEOUT 30)
        add_test(NAME physx34_profile COMMAND pvd_profile_tests
            ${PROJECT_BINARY_DIR}/fixtures/profile_capture.pxd2
            ${PROJECT_BINARY_DIR}/fixtures/profile_expected.csv)
        set_tests_properties(physx34_profile PROPERTIES FIXTURES_REQUIRED physx34_profile TIMEOUT 30)
        add_executable(pvd_physx34_memory tests/physx34_memory.cpp)
        target_link_libraries(pvd_physx34_memory PRIVATE pvd::analysis)
        add_test(NAME physx34_memory_generate COMMAND pvd_physx34_memory_capture
            ${PROJECT_BINARY_DIR}/fixtures/allocations)
        set_tests_properties(physx34_memory_generate PROPERTIES FIXTURES_SETUP physx34_memory TIMEOUT 30)
        add_test(NAME physx34_memory COMMAND pvd_physx34_memory
            ${PROJECT_BINARY_DIR}/fixtures/allocations_memory.pxd2
            ${PROJECT_BINARY_DIR}/fixtures/allocations_memory_expected.csv
            ${PROJECT_BINARY_DIR}/fixtures/allocations_memory_summary.csv)
        set_tests_properties(physx34_memory PROPERTIES FIXTURES_REQUIRED physx34_memory TIMEOUT 30)
        add_test(NAME physx34_curves COMMAND pvd_curves_tests
            ${PROJECT_BINARY_DIR}/fixtures/physical_capture.pxd2)
        set_tests_properties(physx34_curves PROPERTIES FIXTURES_REQUIRED physx34_capture TIMEOUT 30)
        add_test(NAME physx34_memory_bootstrap COMMAND pvd_memory_bootstrap_tests
            ${PROJECT_BINARY_DIR}/fixtures/allocations_memory.pxd2)
        set_tests_properties(physx34_memory_bootstrap PROPERTIES FIXTURES_REQUIRED physx34_memory TIMEOUT 30)
        add_test(NAME physx34_profile_bootstrap COMMAND pvd_profile_bootstrap_tests
            ${PROJECT_BINARY_DIR}/fixtures/profile_capture.pxd2)
        set_tests_properties(physx34_profile_bootstrap PROPERTIES FIXTURES_REQUIRED physx34_profile TIMEOUT 30)
        add_test(NAME physx34_writer_analysis COMMAND pvd_writer_tests
            ${PROJECT_BINARY_DIR}/fixtures/profile_capture.pxd2
            ${PROJECT_BINARY_DIR}/fixtures/allocations_memory.pxd2)
        set_tests_properties(physx34_writer_analysis PROPERTIES FIXTURES_REQUIRED "physx34_profile;physx34_memory" TIMEOUT 30)
    endif()
endif()
