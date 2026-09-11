#pragma once

// Minimal shared NVML glue: dlopen the Nvidia driver's libnvidia-ml at runtime
// (no link dependency, no per-tick fork) and read the first GPU's utilization /
// temperature / VRAM. A box with no driver -> ok() is false and callers hide
// their GPU widgets. The signatures/structs are the stable NVML C ABI; we only
// need a handful. Used by both sysmon/graph (util + VRAM graphs) and hw/gauge
// (the GPU temp dial), so it lives here rather than duplicated in each.

#include <dlfcn.h>

namespace waybar {

struct nvmlUtil {
  unsigned int gpu, memory;
};
struct nvmlMem {
  unsigned long long total, free, used;
};

class Nvml {
 public:
  static Nvml& get() {
    static Nvml n;
    return n;
  }
  bool ok() {
    ensure();
    return ok_;
  }
  double util() {
    nvmlUtil u{};
    return (ok() && getUtil_(dev_, &u) == 0) ? u.gpu : 0.0;
  }
  double temp() {
    unsigned int t = 0;
    return (ok() && getTemp_(dev_, 0 /*NVML_TEMPERATURE_GPU*/, &t) == 0)
               ? t : 0.0;
  }
  double mem_pct() {
    nvmlMem m{};
    return (ok() && getMem_(dev_, &m) == 0 && m.total)
               ? 100.0 * static_cast<double>(m.used) / m.total
               : 0.0;
  }

 private:
  void ensure() {
    if (tried_) return;
    tried_ = true;
    lib_ = dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!lib_) return;
    auto init = reinterpret_cast<int (*)()>(dlsym(lib_, "nvmlInit_v2"));
    auto getdev = reinterpret_cast<int (*)(unsigned int, void**)>(
        dlsym(lib_, "nvmlDeviceGetHandleByIndex_v2"));
    getUtil_ = reinterpret_cast<int (*)(void*, nvmlUtil*)>(
        dlsym(lib_, "nvmlDeviceGetUtilizationRates"));
    getTemp_ = reinterpret_cast<int (*)(void*, int, unsigned int*)>(
        dlsym(lib_, "nvmlDeviceGetTemperature"));
    getMem_ = reinterpret_cast<int (*)(void*, nvmlMem*)>(
        dlsym(lib_, "nvmlDeviceGetMemoryInfo"));
    if (!init || !getdev || !getUtil_ || !getTemp_ || !getMem_) return;
    if (init() != 0) return;
    if (getdev(0, &dev_) != 0 || !dev_) return;
    ok_ = true;
  }
  bool tried_ = false, ok_ = false;
  void* lib_ = nullptr;
  void* dev_ = nullptr;
  int (*getUtil_)(void*, nvmlUtil*) = nullptr;
  int (*getTemp_)(void*, int, unsigned int*) = nullptr;
  int (*getMem_)(void*, nvmlMem*) = nullptr;
};

}  // namespace waybar
