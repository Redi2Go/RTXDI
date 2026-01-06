cmake_minimum_required(VERSION 3.10)

project(RTXDI)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

if (MSVC)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W3 /MP")

    macro(replace_msvcrt var value)
        # Remove the existing /MD-type flags, if any
        string(REGEX REPLACE "/M[TD]d?\\s*" "" ${var} ${${var}})
        # Append the new flag
        set(${var} "${${var}} ${value}")
    endmacro(replace_msvcrt)
    
    replace_msvcrt(CMAKE_C_FLAGS_DEBUG "/MTd")
    replace_msvcrt(CMAKE_C_FLAGS_MINSIZEREL "/MT")
    replace_msvcrt(CMAKE_C_FLAGS_RELEASE "/MT")
    replace_msvcrt(CMAKE_C_FLAGS_RELWITHDEBINFO "/MT")

    replace_msvcrt(CMAKE_CXX_FLAGS_DEBUG "/MTd")
    replace_msvcrt(CMAKE_CXX_FLAGS_MINSIZEREL "/MT")
    replace_msvcrt(CMAKE_CXX_FLAGS_RELEASE "/MT")
    replace_msvcrt(CMAKE_CXX_FLAGS_RELWITHDEBINFO "/MT")

elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")

	# JsonCpp uses its own deprecated methods, which produces annoying warnings.
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-deprecated-declarations")

endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

if (EXISTS "${CMAKE_CURRENT_LIST_DIR}/Libraries/Rtxdi/CMakeLists.txt")
    add_subdirectory(Libraries/Rtxdi)
    set(RTXDI_RUNTIME_INCLUDE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/Libraries/Rtxdi/Include")
endif()

add_subdirectory(Samples/FullSample/Shaders)
add_subdirectory(Samples/FullSample/Source)

if (MSVC)
	set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT FullSample)
endif()
