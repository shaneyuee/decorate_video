include_directories(${OpenCV_libs_DIR}/include)

set(FILTER_SRC
    ${CMAKE_CURRENT_LIST_DIR}/watermark.h ${CMAKE_CURRENT_LIST_DIR}/watermark.cpp
    ${CMAKE_CURRENT_LIST_DIR}/fft_watermark.h ${CMAKE_CURRENT_LIST_DIR}/fft_watermark.cpp PARENT_SCOPE)
