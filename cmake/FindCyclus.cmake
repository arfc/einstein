#  CYCLUS_CORE_FOUND - system has the Cyclus Core library
#  CYCLUS_CORE_INCLUDE_DIR - the Cyclus include directory
#  CYCLUS_CORE_LIBRARIES - The libraries needed to use the Cyclus Core Library
#  CYCLUS_AGENT_TEST_LIBRARIES - A test library for agents
#  CYCLUS_TEST_LIBRARIES - All testing libraries
#  CYCLUS_DEFAULT_TEST_DRIVER - The default cyclus unit test driver

# Check if we have an environment variable for cyclus root
if(DEFINED ENV{CYCLUS_ROOT_DIR})
    if(NOT DEFINED CYCLUS_ROOT_DIR)
        set(CYCLUS_ROOT_DIR "$ENV{CYCLUS_ROOT_DIR}")
    else()
        message(STATUS "\tTwo CYCLUS_ROOT_DIRs have been found:")
        message(STATUS "\t\tThe defined cmake variable CYCLUS_ROOT_DIR: ${CYCLUS_ROOT_DIR}")
        message(STATUS "\t\tThe environment variable CYCLUS_ROOT_DIR: $ENV{CYCLUS_ROOT_DIR}")
    endif()
elseif(DEFINED ENV{CONDA_PREFIX})
    if(NOT DEFINED CYCLUS_ROOT_DIR)
        set(CYCLUS_ROOT_DIR "$ENV{CONDA_PREFIX}")
    endif()
else()
    find_program(CYCLUS_BIN cyclus)
    if(CYCLUS_BIN)
        execute_process(
            COMMAND ${CYCLUS_BIN} --install-path
            OUTPUT_VARIABLE CYCLUS_ROOT_DIR
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
    else()
        message(FATAL_ERROR
            "Could not determine CYCLUS_ROOT_DIR. "
            "Please set CYCLUS_ROOT_DIR or use a Conda environment with Cyclus installed.")
    endif()
endif()

# Let the user know if we're using a hint
message(STATUS "Using ${CYCLUS_ROOT_DIR} as CYCLUS_ROOT_DIR.")

# Use $DEPS_ROOT_DIR if available
if(DEFINED DEPS_ROOT_DIR AND DEPS_ROOT_DIR)
    set(DEPS_CYCLUS "${DEPS_ROOT_DIR}"
                    "${DEPS_ROOT_DIR}/cyclus")
    set(DEPS_LIB_CYCLUS "${DEPS_ROOT_DIR}"
                        "${DEPS_ROOT_DIR}/cyclus"
                        "${DEPS_ROOT_DIR}/lib")
    set(DEPS_SHARE_CYCLUS "${DEPS_ROOT_DIR}/share"
                          "${DEPS_ROOT_DIR}/share/cyclus")
    set(DEPS_INCLUDE_CYCLUS "${DEPS_ROOT_DIR}/include"
                            "${DEPS_ROOT_DIR}/include/cyclus")
else()
    set(DEPS_CYCLUS)
    set(DEPS_LIB_CYCLUS)
    set(DEPS_SHARE_CYCLUS)
    set(DEPS_INCLUDE_CYCLUS)
endif()

message(STATUS "-- Dependency Cyclus (DEPS_CYCLUS): ${DEPS_CYCLUS}")
message(STATUS "-- Dependency Library Cyclus (DEPS_LIB_CYCLUS): ${DEPS_LIB_CYCLUS}")
message(STATUS "-- Dependency Share Cyclus (DEPS_SHARE_CYCLUS): ${DEPS_SHARE_CYCLUS}")
message(STATUS "-- Dependency Include Cyclus (DEPS_INCLUDE_CYCLUS): ${DEPS_INCLUDE_CYCLUS}")

# Set the include dir, this will be the future basis for other defined dirs
find_path(CYCLUS_CORE_INCLUDE_DIR cyclus.h
    HINTS "${CYCLUS_ROOT_DIR}" "${CYCLUS_ROOT_DIR}/cyclus"
          "${CYCLUS_ROOT_DIR}/include"
          "${CYCLUS_ROOT_DIR}/include/cyclus"
          ${DEPS_INCLUDE_CYCLUS}
          /usr/local/cyclus /opt/local/cyclus
    PATH_SUFFIXES cyclus/include include include/cyclus)

# Set the include dir for test headers
find_path(CYCLUS_CORE_TEST_INCLUDE_DIR agent_tests.h
    HINTS "${CYCLUS_ROOT_DIR}" "${CYCLUS_ROOT_DIR}/cyclus/tests"
          "${CYCLUS_ROOT_DIR}/include"
          "${CYCLUS_ROOT_DIR}/include/cyclus/tests"
          ${DEPS_INCLUDE_CYCLUS}
          /usr/local/cyclus /opt/local/cyclus
    PATH_SUFFIXES cyclus/include include include/cyclus include/cyclus/tests cyclus/include/tests)

# Add the root dir to the hints
set(CYCLUS_ROOT_DIR "${CYCLUS_CORE_INCLUDE_DIR}/../..")

# Look for the shared data files
find_path(CYCLUS_CORE_SHARE_DIR cyclus.rng.in
    HINTS "${CYCLUS_ROOT_DIR}" "${CYCLUS_ROOT_DIR}/cyclus"
          "${CYCLUS_ROOT_DIR}/share" "${CYCLUS_ROOT_DIR}/share/cyclus"
          ${DEPS_SHARE_CYCLUS}
          /usr/local/cyclus /opt/local/cyclus
    PATH_SUFFIXES cyclus/share share)

# Look for the main library
find_library(CYCLUS_CORE_LIBRARY NAMES cyclus
    HINTS "${CYCLUS_ROOT_DIR}" "${CYCLUS_ROOT_DIR}/cyclus"
          ${DEPS_CYCLUS}
          /usr/local/cyclus/lib /usr/local/cyclus
          /opt/local /opt/local/cyclus
    PATH_SUFFIXES cyclus/lib lib)

# Optional libraries, only present if Cyclus was built with Cython support
find_library(CYCLUS_EVENTHOOKS_LIBRARY NAMES eventhooks
    HINTS "${CYCLUS_ROOT_DIR}" "${CYCLUS_ROOT_DIR}/cyclus"
          ${DEPS_CYCLUS}
          /usr/local/cyclus/lib /usr/local/cyclus
          /opt/local /opt/local/cyclus
    PATH_SUFFIXES cyclus/lib lib)

find_library(CYCLUS_PYINFILE_LIBRARY NAMES pyinfile
    HINTS "${CYCLUS_ROOT_DIR}" "${CYCLUS_ROOT_DIR}/cyclus"
          ${DEPS_CYCLUS}
          /usr/local/cyclus/lib /usr/local/cyclus
          /opt/local /opt/local/cyclus
    PATH_SUFFIXES cyclus/lib lib)

find_library(CYCLUS_PYMODULE_LIBRARY NAMES pymodule
    HINTS "${CYCLUS_ROOT_DIR}" "${CYCLUS_ROOT_DIR}/cyclus"
          ${DEPS_CYCLUS}
          /usr/local/cyclus/lib /usr/local/cyclus
          /opt/local /opt/local/cyclus
    PATH_SUFFIXES cyclus/lib lib)

# Look for the test libraries
find_library(CYCLUS_AGENT_TEST_LIBRARY NAMES baseagentunittests
    HINTS "${CYCLUS_ROOT_DIR}" "${CYCLUS_ROOT_DIR}/cyclus"
          ${DEPS_CYCLUS}
          /usr/local/cyclus/lib /usr/local/cyclus
          /opt/local /opt/local/cyclus
    PATH_SUFFIXES cyclus/lib lib lib/cyclus)

find_library(CYCLUS_GTEST_LIBRARY NAMES gtest
    HINTS "${CYCLUS_ROOT_DIR}/lib/cyclus"
          "${CYCLUS_ROOT_DIR}" "${CYCLUS_ROOT_DIR}/cyclus"
          "${CYCLUS_ROOT_DIR}/lib" "${CYCLUS_CORE_SHARE_DIR}/../lib"
          ${DEPS_LIB_CYCLUS}
          /usr/local/cyclus/lib /usr/local/cyclus
          /opt/local/lib /opt/local/cyclus/lib
    PATH_SUFFIXES cyclus/lib lib)

# Copy the results to the output variables.
if(CYCLUS_CORE_INCLUDE_DIR AND CYCLUS_CORE_TEST_INCLUDE_DIR
   AND CYCLUS_CORE_LIBRARY AND CYCLUS_GTEST_LIBRARY
   AND CYCLUS_CORE_SHARE_DIR AND CYCLUS_AGENT_TEST_LIBRARY)

    set(CYCLUS_CORE_FOUND 1)
    set(CYCLUS_CORE_LIBRARIES "${CYCLUS_CORE_LIBRARY}")

    # If Cyclus was installed without Cython, these may not exist.
    if(NOT "${CYCLUS_EVENTHOOKS_LIBRARY}" STREQUAL "CYCLUS_EVENTHOOKS_LIBRARY-NOTFOUND")
        list(APPEND CYCLUS_CORE_LIBRARIES "${CYCLUS_EVENTHOOKS_LIBRARY}")
    endif()

    if(NOT "${CYCLUS_PYINFILE_LIBRARY}" STREQUAL "CYCLUS_PYINFILE_LIBRARY-NOTFOUND")
        list(APPEND CYCLUS_CORE_LIBRARIES "${CYCLUS_PYINFILE_LIBRARY}")
    endif()

    if(NOT "${CYCLUS_PYMODULE_LIBRARY}" STREQUAL "CYCLUS_PYMODULE_LIBRARY-NOTFOUND")
        list(APPEND CYCLUS_CORE_LIBRARIES "${CYCLUS_PYMODULE_LIBRARY}")
    endif()

    set(CYCLUS_TEST_LIBRARIES "${CYCLUS_GTEST_LIBRARY}" "${CYCLUS_AGENT_TEST_LIBRARY}")
    set(CYCLUS_AGENT_TEST_LIBRARIES "${CYCLUS_AGENT_TEST_LIBRARY}")
    set(CYCLUS_CORE_INCLUDE_DIRS "${CYCLUS_CORE_INCLUDE_DIR}")
    set(CYCLUS_CORE_TEST_INCLUDE_DIRS "${CYCLUS_CORE_TEST_INCLUDE_DIR}")
    set(CYCLUS_CORE_SHARE_DIRS "${CYCLUS_CORE_SHARE_DIR}")
    set(CYCLUS_DEFAULT_TEST_DRIVER "${CYCLUS_CORE_SHARE_DIR}/cyclus_default_unit_test_driver.cc")
else()
    set(CYCLUS_CORE_FOUND 0)
    set(CYCLUS_CORE_LIBRARIES)
    set(CYCLUS_TEST_LIBRARIES)
    set(CYCLUS_AGENT_TEST_LIBRARIES)
    set(CYCLUS_CORE_INCLUDE_DIRS)
    set(CYCLUS_CORE_TEST_INCLUDE_DIRS)
    set(CYCLUS_CORE_SHARE_DIRS)
    set(CYCLUS_DEFAULT_TEST_DRIVER)
endif()

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CYCLUS_CORE_INCLUDE_DIR}/../../share/cyclus/cmake)

# Report the results.
if(CYCLUS_CORE_FOUND)
    set(CYCLUS_CORE_DIR_MESSAGE "Found Cyclus Core Headers : ${CYCLUS_CORE_INCLUDE_DIRS}")
    set(CYCLUS_CORE_TEST_DIR_MESSAGE "Found Cyclus Core Test Headers : ${CYCLUS_CORE_TEST_INCLUDE_DIRS}")
    set(CYCLUS_CORE_SHARE_MESSAGE "Found Cyclus Core Shared Data : ${CYCLUS_CORE_SHARE_DIRS}")
    set(CYCLUS_CORE_LIB_MESSAGE "Found Cyclus Core Library : ${CYCLUS_CORE_LIBRARIES}")
    set(CYCLUS_TEST_LIB_MESSAGE "Found Cyclus Test Libraries : ${CYCLUS_TEST_LIBRARIES}")
    message(STATUS "${CYCLUS_CORE_DIR_MESSAGE}")
    message(STATUS "${CYCLUS_CORE_TEST_DIR_MESSAGE}")
    message(STATUS "${CYCLUS_CORE_SHARE_MESSAGE}")
    message(STATUS "${CYCLUS_CORE_LIB_MESSAGE}")
    message(STATUS "${CYCLUS_TEST_LIB_MESSAGE}")
else()
    set(CYCLUS_CORE_DIR_MESSAGE
        "Cyclus was not found. Make sure CYCLUS_CORE_LIBRARY and CYCLUS_CORE_INCLUDE_DIR are set.")
    if(NOT Cyclus_FIND_QUIETLY)
        message(STATUS "${CYCLUS_CORE_DIR_MESSAGE}")
        message(STATUS "CYCLUS_CORE_SHARE_DIR was set to : ${CYCLUS_CORE_SHARE_DIR}")
        message(STATUS "CYCLUS_CORE_LIBRARY was set to : ${CYCLUS_CORE_LIBRARY}")
        message(STATUS "CYCLUS_TEST_LIBRARIES was set to : ${CYCLUS_GTEST_LIBRARY}")
        if(Cyclus_FIND_REQUIRED)
            message(FATAL_ERROR "${CYCLUS_CORE_DIR_MESSAGE}")
        endif()
    endif()
endif()

mark_as_advanced(
    CYCLUS_CORE_INCLUDE_DIR
    CYCLUS_CORE_LIBRARY
)