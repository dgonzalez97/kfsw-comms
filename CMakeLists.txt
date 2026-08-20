if(CONFIG_KFSW_CSP)
    if(NOT TARGET csp)
        message(FATAL_ERROR "libcsp was not discovered as a Zephyr module")
    endif()

    add_library(kfsw_comms STATIC
        src/csp.c
    )

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
