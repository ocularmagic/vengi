set(SRCS
  tinyexr.cpp
  tinyexr.h
  miniz.h
)
add_library(tinyexr STATIC ${SRCS})
target_include_directories(tinyexr PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
engine_target_optimize(tinyexr)
