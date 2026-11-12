if(NOT ZT_STAGING_PREFIX OR NOT ZT_HOST_PROTOC)
    message(FATAL_ERROR "Configure through the superbuild, or provide ZT_STAGING_PREFIX and ZT_HOST_PROTOC.")
endif()
if(NOT EXISTS "${ZT_HOST_PROTOC}")
    message(FATAL_ERROR "Host protoc does not exist: ${ZT_HOST_PROTOC}")
endif()
# These application libraries must come from our pinned source builds, not host packages.
find_package(Protobuf 3.21.12.0 EXACT CONFIG REQUIRED
    PATHS "${ZT_STAGING_PREFIX}/lib/cmake/protobuf" NO_DEFAULT_PATH)
find_package(eclipse-paho-mqtt-c CONFIG REQUIRED
    PATHS "${ZT_STAGING_PREFIX}/lib/cmake/eclipse-paho-mqtt-c" NO_DEFAULT_PATH)
find_package(PahoMqttCpp CONFIG REQUIRED
    PATHS "${ZT_STAGING_PREFIX}/lib/cmake/PahoMqttCpp" NO_DEFAULT_PATH)
find_package(SDL3 3.2.30 EXACT CONFIG REQUIRED
    PATHS "${ZT_STAGING_PREFIX}/lib/cmake/SDL3" NO_DEFAULT_PATH)
execute_process(COMMAND "${ZT_HOST_PROTOC}" --version
    RESULT_VARIABLE _protoc_status OUTPUT_VARIABLE _protoc_version OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _protoc_status EQUAL 0 OR NOT _protoc_version STREQUAL "libprotoc 3.21.12")
    message(FATAL_ERROR "Expected build-machine protoc 3.21.12, got: ${_protoc_version}")
endif()
