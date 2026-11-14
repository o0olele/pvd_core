# Optional producer used to validate the independent decoder against a real SDK.
# This file never builds or writes to the SDK tree.
set(PHYSX34_ROOT "" CACHE PATH "Root of a local PhysX-3.4 SDK checkout with Debug x64 binaries")
set(_PVD_PHYSX34_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

function(pvd_add_physx34_capture target_name)
    if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "PhysX 3.4 capture currently requires MSVC and Windows x64")
    endif()
    if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        message(FATAL_ERROR "The supplied PhysX SDK has Debug libraries; configure with -DCMAKE_BUILD_TYPE=Debug")
    endif()
    if(NOT PHYSX34_ROOT)
        message(FATAL_ERROR "Set PHYSX34_ROOT to the local PhysX-3.4 SDK root")
    endif()

    set(_physx_lib "${PHYSX34_ROOT}/PhysX_3.4/Lib/vc15win64")
    set(_physx_bin "${PHYSX34_ROOT}/PhysX_3.4/Bin/vc15win64")
    set(_shared_lib "${PHYSX34_ROOT}/PxShared/lib/vc15win64")
    set(_shared_bin "${PHYSX34_ROOT}/PxShared/bin/vc15win64")
    set(_libraries
        "${_physx_lib}/PhysX3DEBUG_x64.lib"
        "${_physx_lib}/PhysX3CommonDEBUG_x64.lib"
        "${_shared_lib}/PxFoundationDEBUG_x64.lib"
        "${_shared_lib}/PxPvdSDKDEBUG_x64.lib")
    set(_runtime_dlls
        "${_physx_bin}/PhysX3DEBUG_x64.dll"
        "${_physx_bin}/PhysX3CommonDEBUG_x64.dll"
        "${_physx_bin}/nvToolsExt64_1.dll"
        "${_shared_bin}/PxFoundationDEBUG_x64.dll"
        "${_shared_bin}/PxPvdSDKDEBUG_x64.dll")
    set(_required_files ${_libraries} ${_runtime_dlls}
        "${PHYSX34_ROOT}/PhysX_3.4/Include/PxPhysicsAPI.h"
        "${PHYSX34_ROOT}/PxShared/src/pvd/include/PxPvdDataStream.h")
    foreach(_required IN LISTS _required_files)
        if(NOT EXISTS "${_required}")
            message(FATAL_ERROR "Missing PhysX 3.4 capture dependency: ${_required}")
        endif()
    endforeach()

    if(ARGC GREATER 1)
        set(_capture_source "${ARGV1}")
    else()
        set(_capture_source "${_PVD_PHYSX34_SOURCE_ROOT}/examples/physx34_capture.cpp")
    endif()
    add_executable(${target_name} "${_capture_source}")
    target_compile_features(${target_name} PRIVATE cxx_std_17)
    # The producer uses /MDd. Avoid the supplied Extensions static archive: that
    # archive was built with /MTd and cannot safely be mixed into this executable.
    set_property(TARGET ${target_name} PROPERTY MSVC_RUNTIME_LIBRARY MultiThreadedDebugDLL)
    target_compile_definitions(${target_name} PRIVATE PX_DEBUG=1 PX_CHECKED=1 _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${target_name} PRIVATE /W4 /permissive-)
    target_include_directories(${target_name} SYSTEM PRIVATE
        "${PHYSX34_ROOT}/PhysX_3.4/Include"
        "${PHYSX34_ROOT}/PxShared/include"
        "${PHYSX34_ROOT}/PxShared/src/pvd/include")
    target_link_libraries(${target_name} PRIVATE ${_libraries})
    foreach(_dll IN LISTS _runtime_dlls)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_dll}" "$<TARGET_FILE_DIR:${target_name}>"
            VERBATIM)
    endforeach()
    # Multi-config generators cannot constrain an individual target's configs.
    # Stop an accidental Release build instead of linking Debug SDK code to it.
    add_custom_command(TARGET ${target_name} PRE_BUILD
        COMMAND "${CMAKE_COMMAND}" -DSELECTED_CONFIG=$<CONFIG>
            -P "${_PVD_PHYSX34_SOURCE_ROOT}/cmake/RequirePhysX34Debug.cmake"
        VERBATIM)
endfunction()

function(pvd_add_physx34_render_capture target_name)
    if(ARGC GREATER 1)
        set(_source "${ARGV1}")
    else()
        set(_source "${_PVD_PHYSX34_SOURCE_ROOT}/examples/physx34_render_capture.cpp")
    endif()
    pvd_add_physx34_capture(${target_name} "${_source}")
    set(_cooking_lib "${PHYSX34_ROOT}/PhysX_3.4/Lib/vc15win64/PhysX3CookingDEBUG_x64.lib")
    set(_cooking_dll "${PHYSX34_ROOT}/PhysX_3.4/Bin/vc15win64/PhysX3CookingDEBUG_x64.dll")
    foreach(_required IN ITEMS "${_cooking_lib}" "${_cooking_dll}")
        if(NOT EXISTS "${_required}")
            message(FATAL_ERROR "Missing PhysX 3.4 rendering fixture dependency: ${_required}")
        endif()
    endforeach()
    target_link_libraries(${target_name} PRIVATE "${_cooking_lib}")
    target_include_directories(${target_name} SYSTEM PRIVATE
        "${_PVD_PHYSX34_SOURCE_ROOT}/examples/physx34_compat"
        "${PHYSX34_ROOT}/PxShared/src/foundation/include")
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_cooking_dll}" "$<TARGET_FILE_DIR:${target_name}>"
        VERBATIM)
endfunction()
