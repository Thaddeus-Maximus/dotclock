# See the build system documentation in IDF programming guide
# for more information about component CMakeLists.txt files.

idf_component_register(
    SRCS main.c encoder.c display.c audio.c webserver.c simple_dns_server.c
         settings.c alarm.c
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver esp_timer esp_driver_dac littlefs
                  esp_http_server esp_netif esp_wifi nvs_flash
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    add_custom_command(
        OUTPUT ${COMPONENT_DIR}/webpage.h
        COMMAND python ${COMPONENT_DIR}/webpage_compile.py
        DEPENDS ${COMPONENT_DIR}/webpage.html ${COMPONENT_DIR}/webpage_compile.py
        WORKING_DIRECTORY ${COMPONENT_DIR}
        COMMENT "Generating webpage.h from webpage.html"
        VERBATIM
    )

    add_custom_target(generate_webpage_h DEPENDS ${COMPONENT_DIR}/webpage.h)
    add_dependencies(${COMPONENT_LIB} generate_webpage_h)
endif()
