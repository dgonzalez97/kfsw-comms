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
