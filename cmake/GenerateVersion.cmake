# Regenerates version_git.h from the nearest vX.Y.Z(-suffix) git tag on every
# build, so MELEE_APP_VERSION (src/pc/version.cpp) can't go stale the way a
# hand-bumped #define did. Invoked as an ALL custom target -- see CMakeLists.txt.
find_package(Git QUIET)

set(_version "")
if (GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --match "v[0-9]*" --dirty
        WORKING_DIRECTORY "${SRC_DIR}"
        OUTPUT_VARIABLE _version
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _result)
    if (NOT _result EQUAL 0)
        set(_version "")
    endif ()
endif ()

set(_content "// Generated at build time by cmake/GenerateVersion.cmake -- do not edit.\n")
if (_version)
    string(APPEND _content "#define MELEE_APP_VERSION \"${_version}\"\n")
endif ()

# Skip the write when unchanged so an untagged rebuild doesn't force
# version.cpp to recompile every single time.
set(_existing "")
if (EXISTS "${OUT_FILE}")
    file(READ "${OUT_FILE}" _existing)
endif ()
if (NOT _existing STREQUAL _content)
    get_filename_component(_out_dir "${OUT_FILE}" DIRECTORY)
    file(MAKE_DIRECTORY "${_out_dir}")
    file(WRITE "${OUT_FILE}" "${_content}")
endif ()
