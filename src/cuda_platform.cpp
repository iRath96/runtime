#include "cuda_platform.h"
#include "runtime.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#ifndef LIBDEVICE_DIR
#define LIBDEVICE_DIR ""
#endif
#ifndef KERNEL_DIR
#define KERNEL_DIR ""
#endif

#define CHECK_NVVM(err, name)  check_nvvm_errors  (err, name, __FILE__, __LINE__)
#define CHECK_NVRTC(err, name) check_nvrtc_errors (err, name, __FILE__, __LINE__)
#define CHECK_CUDA(err, name)  check_cuda_errors  (err, name, __FILE__, __LINE__)

inline void check_cuda_errors(CUresult err, const char* name, const char* file, const int line) {
    if (CUDA_SUCCESS != err) {
        const char* error_name;
        const char* error_string;
        cuGetErrorName(err, &error_name);
        cuGetErrorString(err, &error_string);
        auto msg = std::string(error_name) + ": " + std::string(error_string);
        error("Driver API function % (%) [file %, line %]: %", name, err, file, line, msg);
    }
}

inline void check_nvvm_errors(nvvmResult err, const char* name, const char* file, const int line) {
    if (NVVM_SUCCESS != err)
        error("NVVM API function % (%) [file %, line %]: %", name, err, file, line, nvvmGetErrorString(err));
}

#ifdef CUDA_NVRTC
inline void check_nvrtc_errors(nvrtcResult err, const char* name, const char* file, const int line) {
    if (NVRTC_SUCCESS != err)
        error("NVRTC API function % (%) [file %, line %]: %", name, err, file, line, nvrtcGetErrorString(err));
}
#endif

extern std::atomic<uint64_t> anydsl_kernel_time;

CudaPlatform::CudaPlatform(Runtime* runtime)
    : Platform(runtime)
{
    int device_count = 0, driver_version = 0, nvvm_major = 0, nvvm_minor = 0;

    #ifndef _WIN32
    setenv("CUDA_CACHE_DISABLE", "1", 1);
    #endif

    CUresult err = cuInit(0);
    CHECK_CUDA(err, "cuInit()");

    err = cuDeviceGetCount(&device_count);
    CHECK_CUDA(err, "cuDeviceGetCount()");

    err = cuDriverGetVersion(&driver_version);
    CHECK_CUDA(err, "cuDriverGetVersion()");

    nvvmResult errNvvm = nvvmVersion(&nvvm_major, &nvvm_minor);
    CHECK_NVVM(errNvvm, "nvvmVersion()");

    debug("CUDA Driver Version %.%", driver_version/1000, (driver_version%100)/10);
    #ifdef CUDA_NVRTC
    int nvrtc_major = 0, nvrtc_minor = 0;
    nvrtcResult errNvrtc = nvrtcVersion(&nvrtc_major, &nvrtc_minor);
    CHECK_NVRTC(errNvrtc, "nvrtcVersion()");
    debug("NVRTC Version %.%", nvrtc_major, nvrtc_minor);
    #endif
    debug("NVVM Version %.%", nvvm_major, nvvm_minor);

    devices_.resize(device_count);

    // Initialize devices
    for (int i = 0; i < device_count; ++i) {
        char name[128];

        err = cuDeviceGet(&devices_[i].dev, i);
        CHECK_CUDA(err, "cuDeviceGet()");
        err = cuDeviceGetName(name, 128, devices_[i].dev);
        CHECK_CUDA(err, "cuDeviceGetName()");
        err = cuDeviceGetAttribute(&devices_[i].compute_major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, devices_[i].dev);
        CHECK_CUDA(err, "cuDeviceGetAttribute()");
        err = cuDeviceGetAttribute(&devices_[i].compute_minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, devices_[i].dev);
        CHECK_CUDA(err, "cuDeviceGetAttribute()");

        debug("  (%) %, Compute capability: %.%", i, name, devices_[i].compute_major, devices_[i].compute_minor);

        err = cuCtxCreate(&devices_[i].ctx, CU_CTX_MAP_HOST, devices_[i].dev);
        CHECK_CUDA(err, "cuCtxCreate()");
    }
}

void CudaPlatform::erase_profiles(bool erase_all) {
    std::lock_guard<std::mutex> guard(profile_lock_);
    profiles_.remove_if([=] (const ProfileData* profile) {
        cuCtxPushCurrent(profile->ctx);
        auto status = cuEventQuery(profile->end);
        auto erased = erase_all || status == CUDA_SUCCESS;
        if (erased) {
            float time;
            if (status == CUDA_SUCCESS) {
                cuEventElapsedTime(&time, profile->start, profile->end);
                anydsl_kernel_time.fetch_add(time * 1000);
            }
            cuEventDestroy(profile->start);
            cuEventDestroy(profile->end);
            delete profile;
        }
        cuCtxPopCurrent(NULL);
        return erased;
    });
}

CudaPlatform::~CudaPlatform() {
    erase_profiles(true);
    for (size_t i = 0; i < devices_.size(); i++)
        cuCtxDestroy(devices_[i].ctx);
}

void* CudaPlatform::alloc(DeviceId dev, int64_t size) {
    cuCtxPushCurrent(devices_[dev].ctx);

    CUdeviceptr mem;
    CUresult err = cuMemAlloc(&mem, size);
    CHECK_CUDA(err, "cuMemAlloc()");

    cuCtxPopCurrent(NULL);
    return (void*)mem;
}

void* CudaPlatform::alloc_host(DeviceId dev, int64_t size) {
    cuCtxPushCurrent(devices_[dev].ctx);

    void* mem;
    CUresult err = cuMemHostAlloc(&mem, size, CU_MEMHOSTALLOC_DEVICEMAP);
    CHECK_CUDA(err, "cuMemHostAlloc()");

    cuCtxPopCurrent(NULL);
    return mem;
}

void* CudaPlatform::alloc_unified(DeviceId dev, int64_t size) {
    cuCtxPushCurrent(devices_[dev].ctx);

    CUdeviceptr mem;
    CUresult err = cuMemAllocManaged(&mem, size, CU_MEM_ATTACH_GLOBAL);
    CHECK_CUDA(err, "cuMemAllocManaged()");

    cuCtxPopCurrent(NULL);
    return (void*)mem;
}

void* CudaPlatform::get_device_ptr(DeviceId dev, void* ptr) {
    cuCtxPushCurrent(devices_[dev].ctx);

    CUdeviceptr mem;
    CUresult err = cuMemHostGetDevicePointer(&mem, ptr, 0);
    CHECK_CUDA(err, "cuMemHostGetDevicePointer()");

    cuCtxPopCurrent(NULL);
    return (void*)mem;
}

void CudaPlatform::release(DeviceId dev, void* ptr) {
    cuCtxPushCurrent(devices_[dev].ctx);
    CUresult err = cuMemFree((CUdeviceptr)ptr);
    CHECK_CUDA(err, "cuMemFree()");
    cuCtxPopCurrent(NULL);
}

void CudaPlatform::release_host(DeviceId dev, void* ptr) {
    cuCtxPushCurrent(devices_[dev].ctx);
    CUresult err = cuMemFreeHost(ptr);
    CHECK_CUDA(err, "cuMemFreeHost()");
    cuCtxPopCurrent(NULL);
}

void CudaPlatform::launch_kernel(DeviceId dev,
                                 const char* file, const char* kernel,
                                 const uint32_t* grid, const uint32_t* block,
                                 void** args, const uint32_t*, const KernelArgType*,
                                 uint32_t) {
    cuCtxPushCurrent(devices_[dev].ctx);

    auto func = load_kernel(dev, file, kernel);

    CUevent start, end;
    if (runtime_->profiling_enabled()) {
        erase_profiles(false);
        CHECK_CUDA(cuEventCreate(&start, CU_EVENT_DEFAULT), "cuEventCreate()");
        CHECK_CUDA(cuEventRecord(start, 0), "cuEventRecord()");
    }

    assert(grid[0] > 0 && grid[0] % block[0] == 0 &&
           grid[1] > 0 && grid[1] % block[1] == 0 &&
           grid[2] > 0 && grid[2] % block[2] == 0 &&
           "The grid size is not a multiple of the block size");

    CUresult err = cuLaunchKernel(func,
        grid[0] / block[0],
        grid[1] / block[1],
        grid[2] / block[2],
        block[0], block[1], block[2],
        0, nullptr, args, nullptr);
    CHECK_CUDA(err, "cuLaunchKernel()");

    if (runtime_->profiling_enabled()) {
        CHECK_CUDA(cuEventCreate(&end, CU_EVENT_DEFAULT), "cuEventCreate()");
        CHECK_CUDA(cuEventRecord(end, 0), "cuEventRecord()");
        profiles_.push_front(new ProfileData { this, devices_[dev].ctx, start, end });
    }
    cuCtxPopCurrent(NULL);
}

void CudaPlatform::synchronize(DeviceId dev) {
    auto& cuda_dev = devices_[dev];
    cuCtxPushCurrent(cuda_dev.ctx);

    CUresult err = cuCtxSynchronize();
    CHECK_CUDA(err, "cuCtxSynchronize()");

    cuCtxPopCurrent(NULL);
    erase_profiles(false);
}

void CudaPlatform::copy(DeviceId dev_src, const void* src, int64_t offset_src, DeviceId dev_dst, void* dst, int64_t offset_dst, int64_t size) {
    assert(dev_src == dev_dst);
    unused(dev_dst);

    cuCtxPushCurrent(devices_[dev_src].ctx);

    CUdeviceptr src_mem = (CUdeviceptr)src;
    CUdeviceptr dst_mem = (CUdeviceptr)dst;
    CUresult err = cuMemcpyDtoD(dst_mem + offset_dst, src_mem + offset_src, size);
    CHECK_CUDA(err, "cuMemcpyDtoD()");

    cuCtxPopCurrent(NULL);
}

void CudaPlatform::copy_from_host(const void* src, int64_t offset_src, DeviceId dev_dst, void* dst, int64_t offset_dst, int64_t size) {
    cuCtxPushCurrent(devices_[dev_dst].ctx);

    CUdeviceptr dst_mem = (CUdeviceptr)dst;

    CUresult err = cuMemcpyHtoD(dst_mem + offset_dst, (char*)src + offset_src, size);
    CHECK_CUDA(err, "cuMemcpyHtoD()");

    cuCtxPopCurrent(NULL);
}

void CudaPlatform::copy_to_host(DeviceId dev_src, const void* src, int64_t offset_src, void* dst, int64_t offset_dst, int64_t size) {
    cuCtxPushCurrent(devices_[dev_src].ctx);

    CUdeviceptr src_mem = (CUdeviceptr)src;
    CUresult err = cuMemcpyDtoH((char*)dst + offset_dst, src_mem + offset_src, size);
    CHECK_CUDA(err, "cuMemcpyDtoH()");

    cuCtxPopCurrent(NULL);
}

CUfunction CudaPlatform::load_kernel(DeviceId dev, const std::string& file, const std::string& kernelname) {
    auto& cuda_dev = devices_[dev];

    // Lock the device when the function cache is accessed
    cuda_dev.lock();

    CUmodule mod;
    auto& mod_cache = cuda_dev.modules;
    auto mod_it = mod_cache.find(file);
    if (mod_it == mod_cache.end()) {
        cuda_dev.unlock();

        CUjit_target target_cc =
            (CUjit_target)(cuda_dev.compute_major * 10 +
                           cuda_dev.compute_minor);

        // Find the file extension
        auto ext_pos = file.rfind('.');
        std::string ext = ext_pos != std::string::npos ? file.substr(ext_pos + 1) : "";
        if (ext != "cu" && ext != "nvvm")
            error("Incorrect extension for kernel file '%' (should be '.nvvm' or '.cu')", file);

        // Compile the given file
        if (std::ifstream(file + ".ptx").good()) {
            mod = create_module(dev, file, target_cc, load_ptx(file + ".ptx").c_str());
        } else if (ext == "cu" && std::ifstream(file).good()) {
            mod = compile_cuda(dev, file, target_cc);
        } else if (ext == "nvvm" && std::ifstream(file + ".bc").good()) {
            mod = compile_nvvm(dev, file + ".bc", target_cc);
        } else {
            error("Cannot find kernel file '%'", file);
        }

        cuda_dev.lock();
        mod_cache[file] = mod;
    } else {
        mod = mod_it->second;
    }

    // Checks that the function exists
    auto& func_cache = devices_[dev].functions;
    auto& func_map = func_cache[mod];
    auto func_it = func_map.find(kernelname);

    CUfunction func;
    if (func_it == func_map.end()) {
        cuda_dev.unlock();

        CUresult err = cuModuleGetFunction(&func, mod, kernelname.c_str());
        if (err != CUDA_SUCCESS)
            info("Function '%' is not present in '%'", kernelname, file);
        CHECK_CUDA(err, "cuModuleGetFunction()");
        int regs, cmem, lmem, smem, threads;
        err = cuFuncGetAttribute(&regs, CU_FUNC_ATTRIBUTE_NUM_REGS, func);
        CHECK_CUDA(err, "cuFuncGetAttribute()");
        err = cuFuncGetAttribute(&smem, CU_FUNC_ATTRIBUTE_SHARED_SIZE_BYTES, func);
        CHECK_CUDA(err, "cuFuncGetAttribute()");
        err = cuFuncGetAttribute(&cmem, CU_FUNC_ATTRIBUTE_CONST_SIZE_BYTES, func);
        CHECK_CUDA(err, "cuFuncGetAttribute()");
        err = cuFuncGetAttribute(&lmem, CU_FUNC_ATTRIBUTE_LOCAL_SIZE_BYTES, func);
        CHECK_CUDA(err, "cuFuncGetAttribute()");
        err = cuFuncGetAttribute(&threads, CU_FUNC_ATTRIBUTE_MAX_THREADS_PER_BLOCK, func);
        CHECK_CUDA(err, "cuFuncGetAttribute()");
        debug("Function '%' using % registers, % | % | % bytes shared | constant | local memory allowing up to % threads per block", kernelname, regs, smem, cmem, lmem, threads);

        cuda_dev.lock();
        func_cache[mod][kernelname] = func;
    } else {
        func = func_it->second;
    }

    cuda_dev.unlock();

    return func;
}

std::string CudaPlatform::load_ptx(const std::string& filename) const {
    std::ifstream src_file(filename);
    if (!src_file.is_open())
        error("Can't open PTX source file '%'", filename);

    return std::string(std::istreambuf_iterator<char>(src_file), (std::istreambuf_iterator<char>()));
}

CUmodule CudaPlatform::compile_nvvm(DeviceId dev, const std::string& filename, CUjit_target target_cc) const {
    std::string libdevice_filename = "libdevice.10.bc";

    #if CUDA_VERSION < 9000
    // Select libdevice module according to documentation
    if (target_cc < 30)
        libdevice_filename = "libdevice.compute_20.10.bc";
    else if (target_cc == 30)
        libdevice_filename = "libdevice.compute_30.10.bc";
    else if (target_cc <  35)
        libdevice_filename = "libdevice.compute_20.10.bc";
    else if (target_cc <= 37)
        libdevice_filename = "libdevice.compute_35.10.bc";
    else if (target_cc <  50)
        libdevice_filename = "libdevice.compute_30.10.bc";
    else if (target_cc <= 53)
        libdevice_filename = "libdevice.compute_50.10.bc";
    else
        libdevice_filename = "libdevice.compute_30.10.bc";
    #endif

    std::ifstream libdevice_file(std::string(LIBDEVICE_DIR) + libdevice_filename);
    if (!libdevice_file.is_open())
        error("Can't open libdevice source file '%'", libdevice_filename);

    std::string libdevice_string = std::string(std::istreambuf_iterator<char>(libdevice_file), (std::istreambuf_iterator<char>()));

    std::ifstream src_file(std::string(KERNEL_DIR) + filename);
    if (!src_file.is_open())
        error("Can't open NVVM source file '%/%'", KERNEL_DIR, filename);

    std::string src_string = std::string(std::istreambuf_iterator<char>(src_file), (std::istreambuf_iterator<char>()));

    nvvmProgram program;
    nvvmResult err = nvvmCreateProgram(&program);
    CHECK_NVVM(err, "nvvmCreateProgram()");

    err = nvvmAddModuleToProgram(program, libdevice_string.c_str(), libdevice_string.length(), libdevice_filename.c_str());
    CHECK_NVVM(err, "nvvmAddModuleToProgram()");

    err = nvvmAddModuleToProgram(program, src_string.c_str(), src_string.length(), filename.c_str());
    CHECK_NVVM(err, "nvvmAddModuleToProgram()");

    std::string compute_arch("-arch=compute_" + std::to_string(target_cc));
    int num_options = 2;
    const char* options[] = {
        compute_arch.c_str(),
        "-opt=3",
        "-g",
        "-generate-line-info" };

    debug("Compiling NVVM to PTX for '%' on CUDA device %", filename, dev);
    err = nvvmCompileProgram(program, num_options, options);
    if (err != NVVM_SUCCESS) {
        size_t log_size;
        nvvmGetProgramLogSize(program, &log_size);
        std::string error_log(log_size, '\0');
        nvvmGetProgramLog(program, &error_log[0]);
        info("Compilation error: %", error_log);
    }
    CHECK_NVVM(err, "nvvmCompileProgram()");

    size_t ptx_size;
    err = nvvmGetCompiledResultSize(program, &ptx_size);
    CHECK_NVVM(err, "nvvmGetCompiledResultSize()");

    std::string ptx(ptx_size, '\0');
    err = nvvmGetCompiledResult(program, &ptx[0]);
    CHECK_NVVM(err, "nvvmGetCompiledResult()");

    err = nvvmDestroyProgram(&program);
    CHECK_NVVM(err, "nvvmDestroyProgram()");

    return create_module(dev, filename, target_cc, ptx.c_str());
}

#ifdef CUDA_NVRTC
#ifndef NVCC_INC
#define NVCC_INC "/usr/local/cuda/include"
#endif
CUmodule CudaPlatform::compile_cuda(DeviceId dev, const std::string& filename, CUjit_target target_cc) const {
    std::ifstream src_file(std::string(KERNEL_DIR) + filename);
    if (!src_file.is_open())
        error("Can't open CUDA source file '%/%'", KERNEL_DIR, filename);

    std::string src_string = std::string(std::istreambuf_iterator<char>(src_file), (std::istreambuf_iterator<char>()));

    nvrtcProgram program;
    nvrtcResult err = nvrtcCreateProgram(&program, src_string.c_str(), filename.c_str(), 0, NULL, NULL);
    CHECK_NVRTC(err, "nvrtcCreateProgram()");

    std::string compute_arch("-arch=compute_" + std::to_string(target_cc));
    int num_options = 3;
    const char* options[] = {
        compute_arch.c_str(),
        "-I",
        NVCC_INC,
        "-G",
        "-lineinfo" };

    debug("Compiling CUDA to PTX for '%' on CUDA device %", filename, dev);
    err = nvrtcCompileProgram(program, num_options, options);
    if (err != NVRTC_SUCCESS) {
        size_t log_size;
        nvrtcGetProgramLogSize(program, &log_size);
        std::string error_log(log_size, '\0');
        nvrtcGetProgramLog(program, &error_log[0]);
        info("Compilation error: %", error_log);
    }
    CHECK_NVRTC(err, "nvrtcCompileProgram()");

    size_t ptx_size;
    err = nvrtcGetPTXSize(program, &ptx_size);
    CHECK_NVRTC(err, "nvrtcGetPTXSize()");

    std::string ptx(ptx_size, '\0');
    err = nvrtcGetPTX(program, &ptx[0]);
    CHECK_NVRTC(err, "nvrtcGetPTX()");

    err = nvrtcDestroyProgram(&program);
    CHECK_NVRTC(err, "nvrtcDestroyProgram()");

    return create_module(dev, filename, target_cc, ptx.c_str());
}
#else
#ifndef NVCC_BIN
#define NVCC_BIN "nvcc"
#endif
CUmodule CudaPlatform::compile_cuda(DeviceId dev, const std::string& filename, CUjit_target target_cc) const {
    #if CUDA_VERSION < 9000
    target_cc = target_cc == CU_TARGET_COMPUTE_21 ? CU_TARGET_COMPUTE_20 : target_cc; // compute_21 does not exist for nvcc
    #endif
    std::string ptx_filename = std::string(filename) + ".ptx";
    std::string command = (NVCC_BIN " -O4 -ptx -arch=compute_") + std::to_string(target_cc) + " ";
    command += std::string(KERNEL_DIR) + filename + " -o " + ptx_filename + " 2>&1";

    debug("Compiling CUDA to PTX for '%' on CUDA device %", filename, dev);
    if (auto stream = popen(command.c_str(), "r")) {
        std::string log;
        char buffer[256];

        while (fgets(buffer, 256, stream))
            log += buffer;

        int exit_status = pclose(stream);
        if (!WEXITSTATUS(exit_status))
            info("%", log);
        else
            error("Compilation error: %", log);
    } else {
        error("Cannot run NVCC");
    }

    return create_module(dev, filename, target_cc, load_ptx(ptx_filename).c_str());
}
#endif

CUmodule CudaPlatform::create_module(DeviceId dev, const std::string& filename, CUjit_target target_cc, const void* ptx) const {
    const unsigned int opt_level = 4;
    const unsigned int error_log_size = 10240;
    const unsigned int num_options = 4;
    char error_log_buffer[error_log_size] = { 0 };

    CUjit_option options[] = { CU_JIT_ERROR_LOG_BUFFER, CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES, CU_JIT_TARGET, CU_JIT_OPTIMIZATION_LEVEL };
    void* option_values[]  = { (void*)error_log_buffer, (void*)error_log_size, (void*)target_cc, (void*)opt_level };

    debug("Creating module from PTX '%' on CUDA device %", filename, dev);
    CUmodule mod;
    CUresult err = cuModuleLoadDataEx(&mod, ptx, num_options, options, option_values);
    if (err != CUDA_SUCCESS)
        info("Compilation error: %", error_log_buffer);
    CHECK_CUDA(err, "cuModuleLoadDataEx()");

    return mod;
}
