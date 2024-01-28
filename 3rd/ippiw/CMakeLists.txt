# ----------------------------------------------------------------------------
#  CMake file for IPP IW. See root CMakeLists.txt
#
# ----------------------------------------------------------------------------
set(IPP_IW_LIBRARY ippiw)
set(IPP_IW_PATH ./)

project(${IPP_IW_LIBRARY})

include_directories(${IPP_INCLUDE_DIRS} ${IPP_IW_PATH}/include)
add_definitions(-DIW_BUILD)
add_definitions(-DICV_BASE)

file(GLOB lib_srcs ${IPP_IW_PATH}/src/*.c)
file(GLOB lib_hdrs ${IPP_IW_PATH}/include/*.h ${IPP_IW_PATH}/include/iw/*.h ${IPP_IW_PATH}/include/iw++/*.hpp)

# ----------------------------------------------------------------------------------
#         Define the library target:
# ----------------------------------------------------------------------------------

add_library(${IPP_IW_LIBRARY} ${lib_srcs} ${lib_hdrs})

if(UNIX)
  if(CV_GCC OR CV_CLANG OR CV_ICC)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-unused-function -Wno-missing-braces -Wno-missing-field-initializers")
  endif()
  if(CV_CLANG)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-self-assign -Wno-strict-prototypes")
  endif()
endif()

set_target_properties(${IPP_IW_LIBRARY}
  PROPERTIES OUTPUT_NAME ${IPP_IW_LIBRARY}
  COMPILE_PDB_NAME ${IPP_IW_LIBRARY}
  ARCHIVE_OUTPUT_DIRECTORY "lib"
  )

