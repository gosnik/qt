if(NOT DEFINED LOADER OR NOT DEFINED PACKAGE_PATH)
    message(FATAL_ERROR "LOADER and PACKAGE_PATH are required")
endif()

file(READ "${LOADER}" loader_content)
get_filename_component(package_name "${PACKAGE_PATH}" NAME)
string(REPLACE "${PACKAGE_PATH}" "${package_name}" normalized_content "${loader_content}")
# Fail closed if a different absolute package path survives normalization.
# Already-normalized loaders are valid, so this check is idempotent.
if(normalized_content MATCHES "[\"']/[^\"']*index[.]data[\"']" OR
   normalized_content MATCHES "[\"'][A-Za-z]:[^\"']*index[.]data[\"']")
    message(FATAL_ERROR "Dashboard loader contains an absolute preload package path")
endif()
if(NOT normalized_content STREQUAL loader_content)
    file(WRITE "${LOADER}" "${normalized_content}")
endif()
