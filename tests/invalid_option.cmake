execute_process(COMMAND "${PROGRAM}" --invalid
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result STREQUAL "2" OR NOT error MATCHES "Invalid arguments" OR NOT output STREQUAL "")
    message(FATAL_ERROR "Unexpected status or output: ${result}, ${error}")
endif()
