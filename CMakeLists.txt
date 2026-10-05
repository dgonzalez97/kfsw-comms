if(CONFIG_KFSW_CSP)
    if(NOT TARGET csp)
        message(FATAL_ERROR "libcsp was not discovered as a Zephyr module")
    endif()

    set(kfsw_comms_sources
        src/csp.c
    )

    if(CONFIG_KFSW_CSP_KISS_UART)
        list(APPEND kfsw_comms_sources src/uart.c)
    endif()

    if(CONFIG_KFSW_CSP_UART_CODEC)
        list(APPEND kfsw_comms_sources src/uart_codec.c)
        set_property(SOURCE
            "${CMAKE_CURRENT_LIST_DIR}/third_party/libcsp/src/interfaces/csp_if_kiss.c"
            TARGET_DIRECTORY csp APPEND PROPERTY COMPILE_DEFINITIONS
            csp_qfifo_write=kfsw_uart_codec_receive
        )
    endif()

    if(CONFIG_KFSW_CSP_CAN)
        list(APPEND kfsw_comms_sources src/can.c)
    endif()

    add_library(kfsw_comms STATIC ${kfsw_comms_sources})

    target_include_directories(kfsw_comms
        PUBLIC
            ${CMAKE_CURRENT_LIST_DIR}/include
    )

    target_link_libraries(kfsw_comms
        PUBLIC
            kfsw::platform
            csp
            zephyr_interface
    )

    # The libcsp tag, so csp info names the library this image runs.
    get_target_property(kfsw_libcsp_dir csp SOURCE_DIR)
    set(kfsw_libcsp_revision "unknown")
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --always
            WORKING_DIRECTORY "${kfsw_libcsp_dir}"
            OUTPUT_VARIABLE kfsw_libcsp_describe
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE kfsw_libcsp_result
        )
        if(kfsw_libcsp_result EQUAL 0 AND kfsw_libcsp_describe)
            set(kfsw_libcsp_revision "${kfsw_libcsp_describe}")
        endif()
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --git-dir
            WORKING_DIRECTORY "${kfsw_libcsp_dir}"
            OUTPUT_VARIABLE kfsw_libcsp_git_dir
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        # Reconfigure when the pin moves.
        if(kfsw_libcsp_git_dir AND EXISTS "${kfsw_libcsp_dir}/${kfsw_libcsp_git_dir}/HEAD")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                "${kfsw_libcsp_dir}/${kfsw_libcsp_git_dir}/HEAD")
        endif()
    endif()
    target_compile_definitions(kfsw_comms
        PRIVATE
            KFSW_LIBCSP_REVISION="${kfsw_libcsp_revision}"
    )
else()
    add_library(kfsw_comms INTERFACE)

    target_include_directories(kfsw_comms
        INTERFACE
            ${CMAKE_CURRENT_LIST_DIR}/include
    )

    target_link_libraries(kfsw_comms
        INTERFACE
            kfsw::platform
            zephyr_interface
    )
endif()

add_library(kfsw::comms ALIAS kfsw_comms)
