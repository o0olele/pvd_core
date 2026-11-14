# Analysis is independent of transport, rendering, ImGui, Windows and the SDK.
add_library(pvd_analysis src/profile.cpp)
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
    add_executable(pvd_profile_tests tests/profile_tests.cpp)
    target_link_libraries(pvd_profile_tests PRIVATE pvd::analysis)
    add_test(NAME profile COMMAND pvd_profile_tests)
    set_tests_properties(profile PROPERTIES TIMEOUT 30)
endif()

if(PVD_BUILD_PHYSX34_VALIDATION)
    include(${CMAKE_CURRENT_LIST_DIR}/PhysX34.cmake)
    pvd_add_physx34_capture(pvd_physx34_profile_capture
        ${PROJECT_SOURCE_DIR}/examples/physx34_profile_capture.cpp)
    target_include_directories(pvd_physx34_profile_capture SYSTEM PRIVATE
        ${PROJECT_SOURCE_DIR}/examples/physx34_compat
        ${PHYSX34_ROOT}/PxShared/src/foundation/include
        ${PHYSX34_ROOT}/PxShared/src/pvd/src)
    if(BUILD_TESTING)
        file(MAKE_DIRECTORY ${PROJECT_BINARY_DIR}/fixtures)
        add_test(NAME physx34_profile_generate COMMAND pvd_physx34_profile_capture
            ${PROJECT_BINARY_DIR}/fixtures/profile)
        set_tests_properties(physx34_profile_generate PROPERTIES FIXTURES_SETUP physx34_profile TIMEOUT 30)
        add_test(NAME physx34_profile COMMAND pvd_profile_tests
            ${PROJECT_BINARY_DIR}/fixtures/profile_capture.pxd2
            ${PROJECT_BINARY_DIR}/fixtures/profile_expected.csv)
        set_tests_properties(physx34_profile PROPERTIES FIXTURES_REQUIRED physx34_profile TIMEOUT 30)
    endif()
endif()
