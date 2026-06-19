# Copyright (c) 2018-2026 Conceal Network & Conceal Devs
# MIT

set(POW_SOURCES
  backend.cpp
  GpuPowConfig.cpp
  cpu_backend.cpp
  gpu_result_cache.cpp
  pow_sync_log.cpp
  pow_factory.cpp
  pow_service.cpp
  prefetch_cache.cpp
  opencl/raii.cpp
  opencl/opencl_program_cache.cpp
  opencl/opencl_backend.cpp
  mining/GpuMinerConfig.cpp
  mining/gpu_mine_intensity.cpp
  mining/kernel_loader.cpp
  mining/GpuMiner.cpp
  mining/OpenclMinerDevice.cpp
  mining/gpu_mine_staged_probe.cpp
)

add_library(pow STATIC ${POW_SOURCES})
target_include_directories(pow PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pow PUBLIC crypto CryptoNoteCore Common Logging ${Boost_LIBRARIES})

if(WITH_OPENCL AND OpenCL_FOUND)
  target_compile_definitions(pow PUBLIC CONCEAL_WITH_OPENCL)
  # Host code uses OpenCL 1.2 C API (queues, kernels). Avoid cl_version.h defaulting to 3.0.
  target_compile_definitions(pow PRIVATE CL_TARGET_OPENCL_VERSION=120)
  target_include_directories(pow SYSTEM PUBLIC ${OpenCL_INCLUDE_DIRS})
  target_link_libraries(pow PUBLIC ${OpenCL_LIBRARIES})
  set(_cn_gpu_inner_cl_src "${CMAKE_CURRENT_SOURCE_DIR}/kernels/cn_gpu_inner.cl")
  set(_cn_gpu_inner_cl_bin "${CMAKE_BINARY_DIR}/cn_gpu_inner.cl")
  set(_cn_gpu_keccak_cl_src "${CMAKE_CURRENT_SOURCE_DIR}/kernels/cn_gpu_keccak.cl")
  set(_cn_gpu_keccak_cl_bin "${CMAKE_BINARY_DIR}/cn_gpu_keccak.cl")
  set(_cn_gpu_mine_cl_src "${CMAKE_CURRENT_SOURCE_DIR}/kernels/cn_gpu_mine.cl")
  set(_cn_gpu_mine_cl_bin "${CMAKE_BINARY_DIR}/cn_gpu_mine.cl")
  add_custom_command(
    OUTPUT ${_cn_gpu_inner_cl_bin}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_cn_gpu_inner_cl_src} ${_cn_gpu_inner_cl_bin}
    DEPENDS ${_cn_gpu_inner_cl_src}
    COMMENT "Sync cn_gpu_inner.cl")
  add_custom_command(
    OUTPUT ${_cn_gpu_keccak_cl_bin}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_cn_gpu_keccak_cl_src} ${_cn_gpu_keccak_cl_bin}
    DEPENDS ${_cn_gpu_keccak_cl_src}
    COMMENT "Sync cn_gpu_keccak.cl")
  add_custom_command(
    OUTPUT ${_cn_gpu_mine_cl_bin}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_cn_gpu_mine_cl_src} ${_cn_gpu_mine_cl_bin}
    DEPENDS ${_cn_gpu_mine_cl_src}
    COMMENT "Sync cn_gpu_mine.cl")
  add_custom_target(sync_cn_gpu_inner_cl DEPENDS ${_cn_gpu_inner_cl_bin})
  add_custom_target(sync_cn_gpu_mine_cl DEPENDS ${_cn_gpu_keccak_cl_bin} ${_cn_gpu_mine_cl_bin})
  add_dependencies(pow sync_cn_gpu_inner_cl sync_cn_gpu_mine_cl)
  target_compile_definitions(pow PRIVATE
    CONCEAL_KERNEL_DIR="${CMAKE_BINARY_DIR}"
    CONCEAL_KERNEL_SOURCE="${_cn_gpu_inner_cl_src}"
    CONCEAL_MINE_KERNEL_DIR="${CMAKE_BINARY_DIR}"
    CONCEAL_MINE_KERNEL_SOURCE_KECCAK="${_cn_gpu_keccak_cl_src}"
    CONCEAL_MINE_KERNEL_SOURCE_MINE="${_cn_gpu_mine_cl_src}")
endif()

add_dependencies(pow version)
