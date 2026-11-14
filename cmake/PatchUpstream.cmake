# PatchUpstream.cmake
#
# Included by CMakeLists.txt. The game's code is built from a patched copy of
# its sources in the build folder (AIR_SRC), so external/sonic3air stays
# exactly as Eukaryot publishes it:
#   <build>/air-src/librmx/source
#   <build>/air-src/Oxygen/lemonscript/source
#   <build>/air-src/Oxygen/oxygenengine/source
# are copied from external/sonic3air, then every patches/*.patch is applied in
# name order (GNU patch, -p1, from the repository root of the game). Files
# whose content did not change keep their timestamps, so reconfiguring does not
# rebuild everything. Each patch says what it fixes and why.
#
# Sets AIR_SRC. Needs AIR_ROOT and S3W_ROOT.

find_program(S3W_PATCH_PROGRAM patch REQUIRED)

set(AIR_SRC "${CMAKE_CURRENT_BINARY_DIR}/air-src")
set(_s3w_staging "${CMAKE_CURRENT_BINARY_DIR}/air-src-staging")
set(_s3w_patched_trees "librmx/source" "Oxygen/lemonscript/source" "Oxygen/oxygenengine/source")
file(GLOB _s3w_patches "${S3W_ROOT}/patches/*.patch")
list(SORT _s3w_patches)

# A fresh copy of the upstream trees, patched.
file(REMOVE_RECURSE "${_s3w_staging}")
foreach(tree IN LISTS _s3w_patched_trees)
    get_filename_component(_s3w_parent "${_s3w_staging}/${tree}" DIRECTORY)
    file(COPY "${AIR_ROOT}/${tree}" DESTINATION "${_s3w_parent}")
endforeach()
foreach(patch_file IN LISTS _s3w_patches)
    execute_process(
        COMMAND "${S3W_PATCH_PROGRAM}" -p1 --forward --batch --silent -d "${_s3w_staging}" -i "${patch_file}"
        RESULT_VARIABLE _s3w_patch_result
        OUTPUT_VARIABLE _s3w_patch_output
        ERROR_VARIABLE _s3w_patch_output)
    if(NOT _s3w_patch_result EQUAL 0)
        message(FATAL_ERROR "Could not apply ${patch_file} to external/sonic3air "
                            "(was the submodule updated?):\n${_s3w_patch_output}")
    endif()
    message(STATUS "Applied ${patch_file}")
endforeach()

# Into the build's copy, touching only what changed.
file(GLOB_RECURSE _s3w_staged_files RELATIVE "${_s3w_staging}" "${_s3w_staging}/*")
foreach(relative IN LISTS _s3w_staged_files)
    get_filename_component(_s3w_target_dir "${AIR_SRC}/${relative}" DIRECTORY)
    file(MAKE_DIRECTORY "${_s3w_target_dir}")
    file(COPY_FILE "${_s3w_staging}/${relative}" "${AIR_SRC}/${relative}" ONLY_IF_DIFFERENT)
endforeach()
file(REMOVE_RECURSE "${_s3w_staging}")

# Reconfigure when a patch is added or changed.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_s3w_patches})
