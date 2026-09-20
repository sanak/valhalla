if(NOT EMSCRIPTEN)
  message(FATAL_ERROR "ENABLE_WASM_BINDINGS needs the Emscripten toolchain, e.g. `emcmake cmake ...`")
endif()

# Emscripten links a whole program, not a shared object: the artifact is valhalla.mjs plus
# valhalla.wasm, not a loadable module like the node/python bindings produce.
add_executable(valhalla_wasm ${CMAKE_CURRENT_SOURCE_DIR}/src/valhalla_wasm.cc)

target_link_libraries(valhalla_wasm PRIVATE valhalla)

set_target_properties(valhalla_wasm PROPERTIES
  OUTPUT_NAME "valhalla"
  SUFFIX ".mjs")

target_compile_options(valhalla_wasm PRIVATE -fwasm-exceptions)

target_link_options(valhalla_wasm PRIVATE
  -lembind
  -fwasm-exceptions
  -sMODULARIZE=1
  -sEXPORT_ES6=1
  -sEXPORT_NAME=createValhalla
  -sENVIRONMENT=web,worker,node
  -sALLOW_MEMORY_GROWTH=1
  -sINITIAL_MEMORY=64MB
  -sMAXIMUM_MEMORY=2GB
  -sSTACK_SIZE=4MB
  -sFORCE_FILESYSTEM=1
  # tiles can come from a host dir (node), a Blob/File (browser worker) or IndexedDB
  -lnodefs.js
  -lworkerfs.js
  -lidbfs.js
  -sEXPORTED_RUNTIME_METHODS=FS,NODEFS,WORKERFS,IDBFS)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/valhalla.mjs
  ${CMAKE_CURRENT_BINARY_DIR}/valhalla.wasm
  ${CMAKE_CURRENT_SOURCE_DIR}/worker.js
  ${CMAKE_CURRENT_SOURCE_DIR}/index.mjs
  ${CMAKE_CURRENT_SOURCE_DIR}/index.d.ts
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/valhalla-wasm")
