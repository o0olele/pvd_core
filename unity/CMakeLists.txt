if(NOT WIN32 OR NOT MSVC OR NOT MSVC_CXX_ARCHITECTURE_ID STREQUAL "x64")
    message(FATAL_ERROR "PVD_BUILD_UNITY_PLUGIN requires Windows x64 and MSVC")
endif()
if(NOT TARGET pvd::render OR NOT TARGET pvd::transport OR BUILD_SHARED_LIBS)
    message(FATAL_ERROR "PVD_BUILD_UNITY_PLUGIN requires PVD_BUILD_RENDER=ON, PVD_BUILD_TRANSPORT=ON, and BUILD_SHARED_LIBS=OFF")
endif()

add_library(pvd_unity SHARED native/pvd_unity.cpp)
target_link_libraries(pvd_unity PRIVATE pvd::render pvd::transport)
target_compile_definitions(pvd_unity PRIVATE PVD_UNITY_BUILD)
target_compile_options(pvd_unity PRIVATE /W4 /permissive- /utf-8)
set_target_properties(pvd_unity PROPERTIES CXX_EXTENSIONS OFF OUTPUT_NAME pvd_unity_v3)

set(package_source "${CMAKE_CURRENT_SOURCE_DIR}/com.pvd.playback")
set(package_manifest "${package_source}/package.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${package_manifest}")
file(READ "${package_manifest}" package_json)
string(JSON package_version GET "${package_json}" version)
set(package_stage "${CMAKE_CURRENT_BINARY_DIR}/stage/${package_version}")
set(package_archive "${PROJECT_SOURCE_DIR}/out/unity/com.pvd.playback-${package_version}.tgz")
add_custom_target(pvd_unity_package
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${PROJECT_SOURCE_DIR}/out/unity"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${package_source}" "${package_stage}/package"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:pvd_unity>"
        "${package_stage}/package/Runtime/Plugins/x86_64/$<TARGET_FILE_NAME:pvd_unity>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PROJECT_SOURCE_DIR}/docs/constraint-visualization-LICENSE.txt"
        "${package_stage}/package/Third Party Notices.txt"
    COMMAND "${CMAKE_COMMAND}" -E chdir "${package_stage}"
        "${CMAKE_COMMAND}" -E tar czf "${package_archive}" --format=gnutar package
    BYPRODUCTS "${package_archive}"
    DEPENDS pvd_unity
    COMMENT "Packing the Unity playback package"
    VERBATIM)
