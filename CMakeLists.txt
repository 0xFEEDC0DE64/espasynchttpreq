set(headers
    src/asynchttprequest.h
)

set(sources
    src/asynchttprequest.cpp
)

set(dependencies
    cpputils
    espchrono
    espcpputils
    esp_http_client
    fmt
)

idf_component_register(
    INCLUDE_DIRS
        src
    SRCS
        ${headers}
        ${sources}
    REQUIRES
        ${dependencies}
)

set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 23)

target_compile_options(${COMPONENT_TARGET}
    PRIVATE
        -fstack-reuse=all
        -fstack-protector-all
        -Wno-unused-function
        -Wno-deprecated-declarations
        -Wno-missing-field-initializers
        -Wno-parentheses
)
