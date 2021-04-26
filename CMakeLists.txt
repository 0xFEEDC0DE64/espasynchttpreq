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

target_compile_options(${COMPONENT_TARGET} PRIVATE -Wno-unused-function -Wno-deprecated-declarations -Wno-missing-field-initializers)
