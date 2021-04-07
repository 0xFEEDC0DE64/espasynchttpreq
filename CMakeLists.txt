set(headers
    src/asynchttprequest.h
)

set(sources
    src/asynchttprequest.cpp
)

idf_component_register(INCLUDE_DIRS
                           src
                       SRCS
                           ${headers}
                           ${sources}
                       REQUIRES
                           cpputils
                           espchrono
                           espcpputils
)

target_compile_options(${COMPONENT_TARGET} PRIVATE -Wno-unused-function -Wno-deprecated-declarations -Wno-missing-field-initializers)
