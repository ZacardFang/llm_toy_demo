# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "D:/esp/esp32_4.4.6/esp-idf/components/bootloader/subproject"
  "D:/esp/Projects/llm_toy_demo/build/bootloader"
  "D:/esp/Projects/llm_toy_demo/build/bootloader-prefix"
  "D:/esp/Projects/llm_toy_demo/build/bootloader-prefix/tmp"
  "D:/esp/Projects/llm_toy_demo/build/bootloader-prefix/src/bootloader-stamp"
  "D:/esp/Projects/llm_toy_demo/build/bootloader-prefix/src"
  "D:/esp/Projects/llm_toy_demo/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "D:/esp/Projects/llm_toy_demo/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
