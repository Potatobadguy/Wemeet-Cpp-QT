# ── Protobuf 协议编译 ──────────────────────────────────
set(PROTO_FILES
    common.proto
    auth.proto
    meeting.proto
    signaling.proto
    media.proto
)

protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})

# 生成 protobuf 库
add_library(wemeet_proto STATIC ${PROTO_SRCS} ${PROTO_HDRS})
target_include_directories(wemeet_proto
    PUBLIC  ${CMAKE_CURRENT_BINARY_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(wemeet_proto PUBLIC protobuf::libprotobuf)
set_target_properties(wemeet_proto PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
