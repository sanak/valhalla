# Injected via -DCMAKE_PROJECT_INCLUDE.
# protoc has to run on the build host, but the wasm protobuf package ships no protoc target,
# so protobuf_generate() would emit the literal string "protobuf::protoc".
if(NOT TARGET protobuf::protoc)
  add_executable(protobuf::protoc IMPORTED GLOBAL)
  set_target_properties(protobuf::protoc PROPERTIES IMPORTED_LOCATION "$ENV{HOST_PROTOC}")
  message(STATUS "wasm: protobuf::protoc -> $ENV{HOST_PROTOC}")
endif()
