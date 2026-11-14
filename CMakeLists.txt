cmake_minimum_required(VERSION 3.20)
project(pvd_core VERSION 0.8.0 LANGUAGES CXX)

option(BUILD_SHARED_LIBS "Build pvd_core as a shared library" OFF)
option(PVD_BUILD_TOOLS "Build the headless capture inspection tool" ON)
option(PVD_BUILD_TRANSPORT "Build the independent TCP capture module" ON)
option(PVD_BUILD_RENDER "Build the UI-independent render extraction module" ON)
option(PVD_BUILD_ANALYSIS "Build independent profiling and engineering analysis" ON)
option(PVD_BUILD_REPX "Build the independent PhysX 3.4 RepX exporter" ${PVD_BUILD_RENDER})
if(WIN32 AND PVD_BUILD_RENDER)
    set(PVD_VIEWER_DEFAULT ON)
else()
    set(PVD_VIEWER_DEFAULT OFF)
endif()
option(PVD_BUILD_VIEWER "Build the native Windows Direct3D 11 viewer" ${PVD_VIEWER_DEFAULT})
option(PVD_BUILD_PHYSX34_VALIDATION "Build tests using an external PhysX 3.4 SDK" OFF)
include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(pvd_core src/protocol.cpp src/session.cpp src/writer.cpp src/coordinates.cpp src/clips.cpp)
add_library(pvd::core ALIAS pvd_core)
set_target_properties(pvd_core PROPERTIES EXPORT_NAME core VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR} CXX_EXTENSIONS OFF)
target_compile_features(pvd_core PUBLIC cxx_std_17)
target_include_directories(pvd_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_definitions(pvd_core PRIVATE PVD_CORE_BUILD)
if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(pvd_core PUBLIC PVD_CORE_STATIC)
endif()
if(MSVC)
    target_compile_options(pvd_core PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(pvd_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(PVD_INSTALL_TARGETS pvd_core)
if(PVD_BUILD_TRANSPORT)
    find_package(Threads REQUIRED)
    add_library(pvd_transport src/capture.cpp)
    add_library(pvd::transport ALIAS pvd_transport)
    set_target_properties(pvd_transport PROPERTIES EXPORT_NAME transport VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR} CXX_EXTENSIONS OFF)
    target_link_libraries(pvd_transport PUBLIC pvd::core PRIVATE Threads::Threads)
    if(WIN32)
        target_link_libraries(pvd_transport PRIVATE ws2_32)
    endif()
    target_compile_definitions(pvd_transport PRIVATE PVD_TRANSPORT_BUILD)
    if(NOT BUILD_SHARED_LIBS)
        target_compile_definitions(pvd_transport PUBLIC PVD_TRANSPORT_STATIC)
    endif()
    if(MSVC)
        target_compile_options(pvd_transport PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(pvd_transport PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    list(APPEND PVD_INSTALL_TARGETS pvd_transport)
endif()

if(PVD_BUILD_RENDER)
    add_library(pvd_render src/render.cpp src/debug_render.cpp src/mesh_builder.cpp src/deformables.cpp
        src/constraint_builder.cpp src/scene_queries.cpp src/body_overlays.cpp src/coordinate_frames.cpp)
    add_library(pvd::render ALIAS pvd_render)
    set_target_properties(pvd_render PROPERTIES EXPORT_NAME render VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR} CXX_EXTENSIONS OFF)
    target_link_libraries(pvd_render PUBLIC pvd::core)
    target_compile_definitions(pvd_render PRIVATE PVD_RENDER_BUILD)
    if(NOT BUILD_SHARED_LIBS)
        target_compile_definitions(pvd_render PUBLIC PVD_RENDER_STATIC)
    endif()
    if(MSVC)
        target_compile_options(pvd_render PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(pvd_render PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    list(APPEND PVD_INSTALL_TARGETS pvd_render)
    install(FILES docs/constraint-visualization-LICENSE.txt
        DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/pvd_render)
endif()

if(PVD_BUILD_ANALYSIS)
    include(cmake/Analysis.cmake)
endif()

if(PVD_BUILD_REPX)
    if(NOT PVD_BUILD_RENDER)
        message(FATAL_ERROR "PVD_BUILD_REPX requires PVD_BUILD_RENDER")
    endif()
    include(cmake/Repx.cmake)
endif()

if(PVD_BUILD_TOOLS)
    add_executable(pvd_inspect tools/pvd_inspect.cpp)
    target_link_libraries(pvd_inspect PRIVATE pvd::core)
    add_executable(pvd_benchmark tools/pvd_benchmark.cpp)
    target_link_libraries(pvd_benchmark PRIVATE pvd::core)
    if(WIN32)
        target_link_libraries(pvd_benchmark PRIVATE psapi)
    endif()
    if(PVD_BUILD_TRANSPORT)
        add_executable(pvd_capture tools/pvd_capture.cpp)
        target_link_libraries(pvd_capture PRIVATE pvd::transport)
    endif()
    if(PVD_BUILD_RENDER)
        add_executable(pvd_scene tools/pvd_scene.cpp)
        target_link_libraries(pvd_scene PRIVATE pvd::render)
    endif()
endif()

if(BUILD_TESTING)
    add_executable(pvd_clip_ranges_tests tests/clip_ranges_tests.cpp)
    target_link_libraries(pvd_clip_ranges_tests PRIVATE pvd::core)
    if(MSVC)
        target_compile_options(pvd_clip_ranges_tests PRIVATE /W4 /utf-8)
    endif()
    add_test(NAME clip_ranges COMMAND pvd_clip_ranges_tests)
    set_tests_properties(clip_ranges PROPERTIES TIMEOUT 30)
    if(PVD_BUILD_RENDER)
        add_executable(pvd_coordinate_frames_tests tests/coordinate_frames_tests.cpp)
        target_link_libraries(pvd_coordinate_frames_tests PRIVATE pvd::render)
        if(MSVC)
            target_compile_options(pvd_coordinate_frames_tests PRIVATE /W4 /utf-8)
        endif()
        add_test(NAME coordinate_frames COMMAND pvd_coordinate_frames_tests)
        set_tests_properties(coordinate_frames PROPERTIES TIMEOUT 30)
    endif()
    add_executable(pvd_coordinates_tests tests/coordinates_tests.cpp)
    target_link_libraries(pvd_coordinates_tests PRIVATE pvd::core)
    if(MSVC)
        target_compile_options(pvd_coordinates_tests PRIVATE /W4 /utf-8)
    endif()
    add_test(NAME coordinates COMMAND pvd_coordinates_tests)
    set_tests_properties(coordinates PROPERTIES TIMEOUT 30)
    add_executable(pvd_writer_tests tests/writer_tests.cpp)
    target_link_libraries(pvd_writer_tests PRIVATE pvd::core)
    add_test(NAME writer COMMAND pvd_writer_tests)
    set_tests_properties(writer PROPERTIES TIMEOUT 30)
    add_executable(pvd_protocol_tests tests/protocol_tests.cpp)
    target_link_libraries(pvd_protocol_tests PRIVATE pvd::core)
    add_test(NAME protocol COMMAND pvd_protocol_tests)
    add_executable(pvd_session_tests tests/session_tests.cpp)
    target_link_libraries(pvd_session_tests PRIVATE pvd::core)
    add_test(NAME session COMMAND pvd_session_tests)
    set_tests_properties(protocol session PROPERTIES TIMEOUT 30)
    if(PVD_BUILD_TRANSPORT)
        add_executable(pvd_capture_tests tests/capture_tests.cpp)
        target_link_libraries(pvd_capture_tests PRIVATE pvd::transport Threads::Threads)
        if(WIN32)
            target_link_libraries(pvd_capture_tests PRIVATE ws2_32)
        endif()
        add_test(NAME capture COMMAND pvd_capture_tests)
        set_tests_properties(capture PROPERTIES TIMEOUT 30)
    endif()
    if(PVD_BUILD_RENDER)
        foreach(render_test IN ITEMS render debug_render mesh_builder deformables constraint_builder scene_queries body_overlays)
            add_executable(pvd_${render_test}_tests tests/${render_test}_tests.cpp)
            target_link_libraries(pvd_${render_test}_tests PRIVATE pvd::render)
            add_test(NAME ${render_test} COMMAND pvd_${render_test}_tests)
            set_tests_properties(${render_test} PROPERTIES TIMEOUT 30)
        endforeach()
    endif()
endif()

if(PVD_BUILD_PHYSX34_VALIDATION)
    include(cmake/PhysX34.cmake)
    pvd_add_physx34_capture(pvd_physx34_capture)
    if(PVD_BUILD_RENDER)
        pvd_add_physx34_render_capture(pvd_physx34_render_capture)
        pvd_add_physx34_render_capture(pvd_physx34_scene_queries_capture
            ${PROJECT_SOURCE_DIR}/examples/physx34_scene_queries_capture.cpp)
        target_compile_options(pvd_physx34_scene_queries_capture PRIVATE /utf-8)
        add_library(pvd_physx34_scene_query_layout OBJECT tests/physx34_scene_query_layout.cpp)
        target_compile_features(pvd_physx34_scene_query_layout PRIVATE cxx_std_17)
        target_compile_definitions(pvd_physx34_scene_query_layout PRIVATE PX_SUPPORT_PVD=1 PX_DEBUG=1 PX_CHECKED=1)
        target_include_directories(pvd_physx34_scene_query_layout SYSTEM PRIVATE
            ${PHYSX34_ROOT}/PhysX_3.4/Include ${PHYSX34_ROOT}/PhysX_3.4/Include/common
            ${PHYSX34_ROOT}/PhysX_3.4/Include/geometry ${PHYSX34_ROOT}/PxShared/include
            ${PHYSX34_ROOT}/PxShared/src/foundation/include ${PHYSX34_ROOT}/PhysX_3.4/Source/Common/src
            ${PHYSX34_ROOT}/PhysX_3.4/Source/PhysX/src ${PROJECT_SOURCE_DIR}/examples/physx34_compat)
        pvd_add_physx34_capture(pvd_physx34_deformables_capture
            ${PROJECT_SOURCE_DIR}/examples/physx34_deformables_capture.cpp)
        set_property(TARGET pvd_physx34_deformables_capture PROPERTY MSVC_RUNTIME_LIBRARY MultiThreadedDebug)
        target_compile_options(pvd_physx34_deformables_capture PRIVATE /utf-8)
        target_link_libraries(pvd_physx34_deformables_capture PRIVATE
            ${PHYSX34_ROOT}/PhysX_3.4/Lib/vc15win64/PhysX3ExtensionsDEBUG.lib)
        pvd_add_physx34_capture(pvd_physx34_joints_capture
            ${PROJECT_SOURCE_DIR}/examples/physx34_joints_capture.cpp)
        set_property(TARGET pvd_physx34_joints_capture PROPERTY MSVC_RUNTIME_LIBRARY MultiThreadedDebug)
        target_compile_options(pvd_physx34_joints_capture PRIVATE /W4 /utf-8)
        target_link_libraries(pvd_physx34_joints_capture PRIVATE
            ${PHYSX34_ROOT}/PhysX_3.4/Lib/vc15win64/PhysX3ExtensionsDEBUG.lib)
        pvd_add_physx34_capture(pvd_physx34_articulation_capture
            ${PROJECT_SOURCE_DIR}/examples/physx34_articulation_capture.cpp)
        set_property(TARGET pvd_physx34_articulation_capture PROPERTY MSVC_RUNTIME_LIBRARY MultiThreadedDebug)
        target_compile_options(pvd_physx34_articulation_capture PRIVATE /utf-8)
        target_link_libraries(pvd_physx34_articulation_capture PRIVATE
            ${PHYSX34_ROOT}/PhysX_3.4/Lib/vc15win64/PhysX3ExtensionsDEBUG.lib)
        pvd_add_physx34_capture(pvd_physx34_constraints
            ${PROJECT_SOURCE_DIR}/tests/physx34_constraints.cpp)
        pvd_add_physx34_capture(pvd_physx34_origin_shift_capture
            ${PROJECT_SOURCE_DIR}/examples/physx34_origin_shift_capture.cpp)
        set_property(TARGET pvd_physx34_origin_shift_capture PROPERTY MSVC_RUNTIME_LIBRARY MultiThreadedDebug)
        target_compile_options(pvd_physx34_origin_shift_capture PRIVATE /utf-8)
        target_link_libraries(pvd_physx34_origin_shift_capture PRIVATE
            ${PHYSX34_ROOT}/PhysX_3.4/Lib/vc15win64/PhysX3ExtensionsDEBUG.lib)
        if(BUILD_TESTING AND PVD_BUILD_REPX)
            add_executable(pvd_physx34_origin_shift tests/physx34_origin_shift.cpp)
            target_link_libraries(pvd_physx34_origin_shift PRIVATE pvd::core pvd::render pvd::repx)
            if(PVD_BUILD_ANALYSIS)
                target_link_libraries(pvd_physx34_origin_shift PRIVATE pvd::analysis)
                target_compile_definitions(pvd_physx34_origin_shift PRIVATE PVD_ORIGIN_TEST_ANALYSIS=1)
            endif()
            target_compile_options(pvd_physx34_origin_shift PRIVATE /W4 /utf-8)
            add_test(NAME physx34_origin_shift_generate COMMAND pvd_physx34_origin_shift_capture
                ${PROJECT_BINARY_DIR}/fixtures/origin_shift)
            set_tests_properties(physx34_origin_shift_generate PROPERTIES FIXTURES_SETUP physx34_origin_shift TIMEOUT 30)
            add_test(NAME physx34_origin_shift COMMAND pvd_physx34_origin_shift
                ${PROJECT_BINARY_DIR}/fixtures/origin_shift_capture.pxd2
                ${PROJECT_BINARY_DIR}/fixtures/origin_shift_expected.csv
                ${PROJECT_BINARY_DIR}/fixtures/origin_shift_export)
            set_tests_properties(physx34_origin_shift PROPERTIES FIXTURES_REQUIRED physx34_origin_shift
                FIXTURES_SETUP physx34_origin_shift_repx TIMEOUT 30)
            foreach(suffix IN ITEMS "" _end _crop)
                add_test(NAME physx34_origin_shift_repx${suffix} COMMAND pvd_physx34_repx_validate
                    ${PROJECT_BINARY_DIR}/fixtures/origin_shift_export${suffix}.repx
                    ${PROJECT_BINARY_DIR}/fixtures/origin_shift_export${suffix}_sdk_expected.csv
                    --oracle ${PROJECT_BINARY_DIR}/fixtures/origin_shift_export${suffix}_official.repx)
                set_tests_properties(physx34_origin_shift_repx${suffix} PROPERTIES
                    FIXTURES_REQUIRED physx34_origin_shift_repx TIMEOUT 30)
            endforeach()
        endif()
        target_link_libraries(pvd_physx34_constraints PRIVATE pvd::render)
        target_include_directories(pvd_physx34_constraints SYSTEM PRIVATE
            ${PROJECT_SOURCE_DIR}/examples/physx34_compat
            ${PHYSX34_ROOT}/PhysX_3.4/Source/Common/src
            ${PHYSX34_ROOT}/PhysX_3.4/Include/common
            ${PHYSX34_ROOT}/PxShared/src/foundation/include)
        target_compile_options(pvd_physx34_constraints PRIVATE /utf-8)
        pvd_add_physx34_capture(pvd_physx34_stress_capture
            ${PROJECT_SOURCE_DIR}/examples/physx34_stress_capture.cpp)
        set_property(TARGET pvd_physx34_stress_capture PROPERTY MSVC_RUNTIME_LIBRARY MultiThreadedDebug)
        target_compile_options(pvd_physx34_stress_capture PRIVATE /W4 /utf-8)
        target_link_libraries(pvd_physx34_stress_capture PRIVATE
            ${PHYSX34_ROOT}/PhysX_3.4/Lib/vc15win64/PhysX3ExtensionsDEBUG.lib)
    endif()
    if(BUILD_TESTING)
        add_executable(pvd_physx34_integration tests/physx34_integration.cpp)
        target_link_libraries(pvd_physx34_integration PRIVATE pvd::core)
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
        add_test(NAME physx34_generate COMMAND pvd_physx34_capture
            "${CMAKE_CURRENT_BINARY_DIR}/fixtures/physical")
        set_tests_properties(physx34_generate PROPERTIES FIXTURES_SETUP physx34_capture)
        add_test(NAME physx34_replay COMMAND pvd_physx34_integration
            "${CMAKE_CURRENT_BINARY_DIR}/fixtures/physical_capture.pxd2"
            "${CMAKE_CURRENT_BINARY_DIR}/fixtures/physical_expected.csv"
            "${CMAKE_CURRENT_BINARY_DIR}/fixtures/physical_frame_expected.csv")
        set_tests_properties(physx34_replay PROPERTIES FIXTURES_REQUIRED physx34_capture)
        add_executable(pvd_physx34_custom tests/physx34_custom.cpp)
        target_link_libraries(pvd_physx34_custom PRIVATE pvd::core)
        add_test(NAME physx34_custom COMMAND pvd_physx34_custom
            "${CMAKE_CURRENT_BINARY_DIR}/fixtures/physical_custom.pxd2"
            "${CMAKE_CURRENT_BINARY_DIR}/fixtures/physical_custom_expected.csv")
        set_tests_properties(physx34_custom PROPERTIES FIXTURES_REQUIRED physx34_capture)
        add_test(NAME physx34_writer COMMAND pvd_writer_tests
            ${CMAKE_CURRENT_BINARY_DIR}/fixtures/physical_capture.pxd2
            ${CMAKE_CURRENT_BINARY_DIR}/fixtures/physical_custom.pxd2)
        set_tests_properties(physx34_writer PROPERTIES FIXTURES_REQUIRED physx34_capture TIMEOUT 30)
        if(PVD_BUILD_RENDER)
            add_executable(pvd_physx34_scene_queries tests/physx34_scene_queries.cpp)
            target_link_libraries(pvd_physx34_scene_queries PRIVATE pvd::render)
            add_test(NAME physx34_scene_queries_generate COMMAND pvd_physx34_scene_queries_capture
                ${PROJECT_BINARY_DIR}/fixtures/queries)
            set_tests_properties(physx34_scene_queries_generate PROPERTIES FIXTURES_SETUP physx34_scene_queries TIMEOUT 30)
            add_test(NAME physx34_scene_queries COMMAND pvd_physx34_scene_queries
                ${PROJECT_BINARY_DIR}/fixtures/queries_capture.pxd2 ${PROJECT_BINARY_DIR}/fixtures/queries_expected.csv)
            set_tests_properties(physx34_scene_queries PROPERTIES FIXTURES_REQUIRED physx34_scene_queries TIMEOUT 30)
            add_test(NAME body_overlays_physx34 COMMAND pvd_body_overlays_tests
                ${PROJECT_BINARY_DIR}/fixtures/physical_capture.pxd2)
            set_tests_properties(body_overlays_physx34 PROPERTIES FIXTURES_REQUIRED physx34_capture TIMEOUT 30)
            add_executable(pvd_physx34_articulation tests/physx34_articulation.cpp)
            target_link_libraries(pvd_physx34_articulation PRIVATE pvd::core pvd::render)
            if(MSVC)
                target_compile_options(pvd_physx34_articulation PRIVATE /W4 /utf-8)
            endif()
            add_test(NAME physx34_articulation_generate COMMAND pvd_physx34_articulation_capture
                ${PROJECT_BINARY_DIR}/fixtures/articulation)
            set_tests_properties(physx34_articulation_generate PROPERTIES FIXTURES_SETUP physx34_articulation TIMEOUT 30)
            add_test(NAME physx34_articulation COMMAND pvd_physx34_articulation
                ${PROJECT_BINARY_DIR}/fixtures/articulation_capture.pxd2 ${PROJECT_BINARY_DIR}/fixtures/articulation_expected.csv)
            set_tests_properties(physx34_articulation PROPERTIES FIXTURES_REQUIRED physx34_articulation TIMEOUT 30)
            add_test(NAME coordinate_frames_physx34 COMMAND pvd_coordinate_frames_tests
                ${PROJECT_BINARY_DIR}/fixtures/articulation_capture.pxd2
                ${PROJECT_BINARY_DIR}/fixtures/articulation_expected.csv)
            set_tests_properties(coordinate_frames_physx34 PROPERTIES FIXTURES_REQUIRED physx34_articulation TIMEOUT 30)
            add_test(NAME physx34_joints_generate COMMAND pvd_physx34_joints_capture
                ${PROJECT_BINARY_DIR}/fixtures/joints)
            set_tests_properties(physx34_joints_generate PROPERTIES FIXTURES_SETUP physx34_joints TIMEOUT 30)
            add_test(NAME physx34_constraints COMMAND pvd_physx34_constraints
                ${PROJECT_BINARY_DIR}/fixtures/joints_capture.pxd2)
            set_tests_properties(physx34_constraints PROPERTIES FIXTURES_REQUIRED physx34_joints TIMEOUT 30)
            add_executable(pvd_physx34_stress tests/physx34_stress.cpp)
            target_link_libraries(pvd_physx34_stress PRIVATE pvd::render)
            add_test(NAME physx34_stress_generate COMMAND pvd_physx34_stress_capture
                ${PROJECT_BINARY_DIR}/fixtures/stress)
            set_tests_properties(physx34_stress_generate PROPERTIES FIXTURES_SETUP physx34_stress TIMEOUT 60)
            add_test(NAME physx34_stress COMMAND pvd_physx34_stress
                ${PROJECT_BINARY_DIR}/fixtures/stress_capture.pxd2
                ${PROJECT_BINARY_DIR}/fixtures/stress_expected.csv)
            set_tests_properties(physx34_stress PROPERTIES FIXTURES_REQUIRED physx34_stress TIMEOUT 60)
            add_test(NAME physx34_deformables_generate COMMAND pvd_physx34_deformables_capture
                ${PROJECT_BINARY_DIR}/fixtures/deformables)
            set_tests_properties(physx34_deformables_generate PROPERTIES FIXTURES_SETUP physx34_deformables TIMEOUT 30)
            add_test(NAME physx34_deformables COMMAND pvd_deformables_tests
                ${PROJECT_BINARY_DIR}/fixtures/deformables_capture.pxd2
                ${PROJECT_BINARY_DIR}/fixtures/deformables_expected.csv)
            set_tests_properties(physx34_deformables PROPERTIES FIXTURES_REQUIRED physx34_deformables TIMEOUT 30)
            add_executable(pvd_physx34_render tests/physx34_render.cpp)
            target_link_libraries(pvd_physx34_render PRIVATE pvd::render)
            add_test(NAME physx34_render_generate COMMAND pvd_physx34_render_capture
                "${CMAKE_CURRENT_BINARY_DIR}/fixtures/render")
            set_tests_properties(physx34_render_generate PROPERTIES FIXTURES_SETUP physx34_render TIMEOUT 30)
            add_test(NAME physx34_render COMMAND pvd_physx34_render
                "${CMAKE_CURRENT_BINARY_DIR}/fixtures/render_capture.pxd2"
                "${CMAKE_CURRENT_BINARY_DIR}/fixtures/render_expected.csv")
            set_tests_properties(physx34_render PROPERTIES FIXTURES_REQUIRED physx34_render TIMEOUT 30)
            add_test(NAME physx34_writer_render COMMAND pvd_writer_tests
                ${CMAKE_CURRENT_BINARY_DIR}/fixtures/render_capture.pxd2)
            set_tests_properties(physx34_writer_render PROPERTIES FIXTURES_REQUIRED physx34_render TIMEOUT 30)
            add_test(NAME physx34_writer_native COMMAND pvd_writer_tests
                ${PROJECT_BINARY_DIR}/fixtures/articulation_capture.pxd2
                ${PROJECT_BINARY_DIR}/fixtures/queries_capture.pxd2
                ${PROJECT_BINARY_DIR}/fixtures/deformables_capture.pxd2)
            set_tests_properties(physx34_writer_native PROPERTIES
                FIXTURES_REQUIRED "physx34_articulation;physx34_scene_queries;physx34_deformables" TIMEOUT 30)
        endif()
        if(PVD_BUILD_TRANSPORT)
            add_executable(pvd_physx34_live tests/physx34_live.cpp)
            target_link_libraries(pvd_physx34_live PRIVATE pvd::transport)
            add_test(NAME physx34_live COMMAND pvd_physx34_live
                "$<TARGET_FILE:pvd_physx34_capture>" "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
            set_tests_properties(physx34_live PROPERTIES TIMEOUT 45)
        endif()
    endif()
endif()

if(PVD_BUILD_VIEWER)
    if(NOT WIN32 OR NOT PVD_BUILD_RENDER)
        message(FATAL_ERROR "PVD_BUILD_VIEWER requires Windows and PVD_BUILD_RENDER")
    endif()
    add_subdirectory(viewer)
endif()

install(TARGETS ${PVD_INSTALL_TARGETS} EXPORT pvd_coreTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT pvd_coreTargets FILE pvd_coreTargets.cmake NAMESPACE pvd::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pvd_core)
configure_package_config_file(cmake/pvd_coreConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/pvd_coreConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pvd_core)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/pvd_coreConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/pvd_coreConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/pvd_coreConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pvd_core)
