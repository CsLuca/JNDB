cmake_minimum_required(VERSION 3.20)

project(NdbDecoder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(ndb_decode
  src/main.cpp
  src/wav.cpp
  src/dsp.cpp
  src/ndb_decoder.cpp
)

if (WIN32)
  target_sources(ndb_decode PRIVATE
    src/gui_win32.cpp
  )
endif()

target_include_directories(ndb_decode
  PRIVATE
    src
)

find_path(FFTW3_INCLUDE_DIR fftw3.h)
find_library(FFTW3F_LIBRARY NAMES fftw3f libfftw3f-3)
if (FFTW3_INCLUDE_DIR AND FFTW3F_LIBRARY)
  target_include_directories(ndb_decode PRIVATE ${FFTW3_INCLUDE_DIR})
  target_link_libraries(ndb_decode PRIVATE ${FFTW3F_LIBRARY})
  target_compile_definitions(ndb_decode PRIVATE NDB_HAS_FFTW3F=1)
endif()

if (WIN32)
  target_link_libraries(ndb_decode
    PRIVATE
      comctl32
      gdiplus
  )
endif()
