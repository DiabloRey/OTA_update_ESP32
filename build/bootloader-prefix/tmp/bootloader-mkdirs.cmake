# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "E:/Espressif/Espressif/frameworks/esp-idf-v5.3.1/components/bootloader/subproject"
  "E:/Espressif/Projects/OTA_update_V1/build/bootloader"
  "E:/Espressif/Projects/OTA_update_V1/build/bootloader-prefix"
  "E:/Espressif/Projects/OTA_update_V1/build/bootloader-prefix/tmp"
  "E:/Espressif/Projects/OTA_update_V1/build/bootloader-prefix/src/bootloader-stamp"
  "E:/Espressif/Projects/OTA_update_V1/build/bootloader-prefix/src"
  "E:/Espressif/Projects/OTA_update_V1/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "E:/Espressif/Projects/OTA_update_V1/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "E:/Espressif/Projects/OTA_update_V1/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
