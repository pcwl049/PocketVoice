#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "QnnBackend.h"
#include "QnnContext.h"
#include "QnnDevice.h"
#include "QnnGraph.h"
#include "QnnInterface.h"
#include "QnnLog.h"
#include "QnnProfile.h"

namespace {

void LogCallback(const char *fmt, QnnLog_Level_t level, uint64_t timestamp,
                 va_list args) {
  std::fprintf(stderr, "[qnn-log level=%d ts=%llu] ",
               static_cast<int>(level),
               static_cast<unsigned long long>(timestamp));
  std::vfprintf(stderr, fmt, args);
  std::fprintf(stderr, "\n");
}

using QnnInterfaceGetProvidersFn =
    Qnn_ErrorHandle_t (*)(const QnnInterface_t ***providers,
                          uint32_t *numProviders);

namespace qnn {
namespace commandline2 {
class ICommandLineManager;
}  // namespace commandline2
namespace tools {
namespace iotensor {
class IBufferAlloc;
}  // namespace iotensor
namespace netrun {

enum class PerfProfile {
  LOW_BALANCED,
  BALANCED,
  DEFAULT,
  HIGH_PERFORMANCE,
  SUSTAINED_HIGH_PERFORMANCE,
  BURST,
  GENAI_BURST,
  GENAI_LOW_BALANCED,
  GENAI_BALANCED,
  GENAI_DEFAULT,
  GENAI_HIGH_PERFORMANCE,
  GENAI_SUSTAINED_HIGH_PERFORMANCE,
  GENAI_EXTREME_POWER_SAVER,
  GENAI_LOW_POWER_SAVER,
  GENAI_POWER_SAVER,
  GENAI_HIGH_POWER_SAVER,
  EXTREME_POWER_SAVER,
  LOW_POWER_SAVER,
  POWER_SAVER,
  HIGH_POWER_SAVER,
  SYSTEM_SETTINGS,
  NO_USER_INPUT,
  CUSTOM,
  INVALID
};

enum class AppType {
  QNN_APP_NETRUN = 0,
  QNN_APP_CONTEXT_BINARY_GENERATOR = 1,
  QNN_APP_UNKNOWN = 0x7FFFFFFF
};

struct GraphConfigInfo_t;

class IBackend {
 public:
  virtual ~IBackend() = default;

  virtual bool setupLogging(QnnLog_Callback_t callback,
                            QnnLog_Level_t maxLogLevel) = 0;
  virtual bool initialize(void *backendLibHandle) = 0;
  virtual bool setPerfProfile(PerfProfile perfProfile) = 0;
  virtual QnnProfile_Level_t getProfilingLevel() = 0;
  virtual bool loadConfig(std::string configFile) = 0;
  virtual bool loadCommandLineArgs(
      std::shared_ptr<commandline2::ICommandLineManager> clManager) = 0;
  virtual bool beforeBackendInitialize(QnnBackend_Config_t ***customConfigs,
                                       uint32_t *configCount) = 0;
  virtual bool afterBackendInitialize() = 0;
  virtual bool beforeContextCreate(QnnContext_Config_t ***customConfigs,
                                   uint32_t *configCount) = 0;
  virtual bool afterContextCreate() = 0;
  virtual bool beforeComposeGraphs(GraphConfigInfo_t ***customGraphConfigs,
                                   uint32_t *graphCount) = 0;
  virtual bool afterComposeGraphs() = 0;
  virtual bool beforeGraphFinalizeUpdateConfig(
      const char *graphName, Qnn_GraphHandle_t graphHandle,
      QnnGraph_Config_t ***customConfigs, uint32_t *configCount) = 0;
  virtual bool beforeGraphFinalize() = 0;
  virtual bool afterGraphFinalize() = 0;
  virtual bool beforeRegisterOpPackages() = 0;
  virtual bool afterRegisterOpPackages() = 0;
  virtual bool beforeExecute(const char *graphName,
                             QnnGraph_Config_t ***customConfigs,
                             uint32_t *configCount) = 0;
  virtual bool afterExecute() = 0;
  virtual bool beforeContextFree(
      const std::vector<Qnn_ContextHandle_t> &contextHandle) = 0;
  virtual bool afterContextFree() = 0;
  virtual bool beforeBackendTerminate() = 0;
  virtual bool afterBackendTerminate() = 0;
  virtual bool beforeCreateFromBinary(QnnContext_Config_t ***customConfigs,
                                      uint32_t *configCount) = 0;
  virtual bool afterCreateFromBinary() = 0;
  virtual bool beforeCreateContextsFromBinaryList(
      void *contextKeyToCustomConfigsMap, QnnContext_Config_t ***commonConfigs,
      uint32_t *commonConfigCount) = 0;
  virtual bool afterCreateContextsFromBinaryList() = 0;
  virtual bool beforeCreateDevice(QnnDevice_Config_t ***deviceConfigs,
                                  uint32_t *configCount,
                                  uint32_t socModel) = 0;
  virtual bool afterCreateDevice() = 0;
  virtual bool beforeFreeDevice() = 0;
  virtual bool afterFreeDevice() = 0;
  virtual bool beforeActivateContext(QnnContext_Config_t ***customConfigs,
                                     uint32_t *configCount) = 0;
  virtual bool afterActivateContext() = 0;
  virtual bool beforeDeactivateContext(QnnContext_Config_t ***customConfigs,
                                       uint32_t *configCount) = 0;
  virtual bool afterDeactivateContext(QnnContext_Config_t ***customConfigs,
                                      uint32_t *configCount) = 0;
  virtual std::unique_ptr<uint8_t[]> allocateBinaryBuffer(
      uint32_t bufferSize) = 0;
  virtual void releaseBinaryBuffer(std::unique_ptr<uint8_t[]> buffer) = 0;
  virtual std::unique_ptr<iotensor::IBufferAlloc> getBufferAllocator() = 0;
  virtual bool setParentAppType(AppType appType) = 0;
  virtual bool beforeContextApplyBinarySection() = 0;
  virtual bool afterContextApplyBinarySection() = 0;
  virtual bool isOpMappingsRequired() = 0;
  virtual bool prepareSoc(std::int32_t curDeviceId, std::string dspArch,
                          int vtcmMem, std::string name) = 0;
  virtual bool allocateExternalBuffers(void *contextHandle,
                                       int64_t scratchBuffer,
                                       int64_t weightsBuffer) = 0;
  virtual void provideOpMappings(Qnn_OpMapping_t *opMappings,
                                 uint32_t numOpMappings) = 0;
  virtual bool detachableBuffersEnabled() = 0;
  virtual bool detachBuffers(Qnn_ContextHandle_t contextHandle) = 0;
  virtual bool attachBuffers(Qnn_ContextHandle_t contextHandle) = 0;
};

using CreateBackendInterfaceFnType = IBackend *(*)();
using DestroyBackendInterfaceFnType = void (*)(IBackend *);

}  // namespace netrun
}  // namespace tools
}  // namespace qnn

}  // namespace

int main(int argc, char **argv) {
  const char *backend_path = argc > 1 ? argv[1] : "libQnnHtp.so";
  const char *extensions_path = argc > 2 ? argv[2] : nullptr;
  const char *extensions_config = argc > 3 ? argv[3] : nullptr;
  uint32_t soc_model = argc > 4 ? static_cast<uint32_t>(std::strtoul(argv[4], nullptr, 10)) : 0;
  std::printf("backend=%s\n", backend_path);
  std::printf("extensions=%s\n", extensions_path ? extensions_path : "");
  std::printf("extensions_config=%s\n", extensions_config ? extensions_config : "");
  std::printf("soc_model=%u\n", soc_model);
  std::printf("ADSP_LIBRARY_PATH=%s\n",
              std::getenv("ADSP_LIBRARY_PATH") ? std::getenv("ADSP_LIBRARY_PATH")
                                                : "");
  std::printf("LD_LIBRARY_PATH=%s\n",
              std::getenv("LD_LIBRARY_PATH") ? std::getenv("LD_LIBRARY_PATH")
                                              : "");

  void *handle = dlopen(backend_path, RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
    return 2;
  }

  auto get_providers = reinterpret_cast<QnnInterfaceGetProvidersFn>(
      dlsym(handle, "QnnInterface_getProviders"));
  if (!get_providers) {
    std::fprintf(stderr, "dlsym QnnInterface_getProviders failed: %s\n",
                 dlerror());
    return 3;
  }

  const QnnInterface_t **providers = nullptr;
  uint32_t num_providers = 0;
  Qnn_ErrorHandle_t ret = get_providers(&providers, &num_providers);
  std::printf("getProviders ret=%lu num=%u providers=%p\n",
              static_cast<unsigned long>(ret), num_providers, providers);
  if (ret != QNN_SUCCESS || !providers || num_providers == 0) return 4;

  QNN_INTERFACE_VER_TYPE qnn = providers[0]->QNN_INTERFACE_VER_NAME;
  Qnn_LogHandle_t log_handle = nullptr;
  ret = qnn.logCreate(LogCallback, QNN_LOG_LEVEL_INFO, &log_handle);
  std::printf("logCreate ret=%lu handle=%p\n", static_cast<unsigned long>(ret),
              log_handle);
  if (ret != QNN_SUCCESS) return 5;

  Qnn_BackendHandle_t backend_handle = nullptr;
  ret = qnn.backendCreate(log_handle, nullptr, &backend_handle);
  std::printf("backendCreate ret=%lu handle=%p\n",
              static_cast<unsigned long>(ret), backend_handle);
  if (ret != QNN_SUCCESS) return 6;

  void *extensions_handle = nullptr;
  qnn::tools::netrun::IBackend *backend_extensions = nullptr;
  qnn::tools::netrun::DestroyBackendInterfaceFnType destroy_backend_interface =
      nullptr;
  QnnDevice_Config_t **extension_device_configs = nullptr;
  uint32_t extension_device_config_count = 0;
  std::vector<const QnnDevice_Config_t *> device_configs;

  if (extensions_path && extensions_config) {
    extensions_handle = dlopen(extensions_path, RTLD_NOW | RTLD_LOCAL);
    if (!extensions_handle) {
      std::fprintf(stderr, "dlopen extensions failed: %s\n", dlerror());
      return 7;
    }
    auto create_backend_interface =
        reinterpret_cast<qnn::tools::netrun::CreateBackendInterfaceFnType>(
            dlsym(extensions_handle, "createBackendInterface"));
    destroy_backend_interface =
        reinterpret_cast<qnn::tools::netrun::DestroyBackendInterfaceFnType>(
            dlsym(extensions_handle, "destroyBackendInterface"));
    if (!create_backend_interface || !destroy_backend_interface) {
      std::fprintf(stderr, "dlsym extensions failed: %s\n", dlerror());
      return 8;
    }
    backend_extensions = create_backend_interface();
    if (!backend_extensions) return 9;
    std::printf("extensions.setupLogging=%d\n",
                backend_extensions->setupLogging(LogCallback, QNN_LOG_LEVEL_INFO));
    std::printf("extensions.initialize=%d\n",
                backend_extensions->initialize(handle));
    std::printf("extensions.loadConfig=%d\n",
                backend_extensions->loadConfig(extensions_config));
    std::printf("extensions.beforeCreateDevice=%d\n",
                backend_extensions->beforeCreateDevice(
                    &extension_device_configs, &extension_device_config_count,
                    soc_model));
    std::printf("extensions.deviceConfigCount=%u configs=%p\n",
                extension_device_config_count, extension_device_configs);
    for (uint32_t i = 0; i < extension_device_config_count; ++i) {
      device_configs.push_back(extension_device_configs[i]);
    }
  }
  device_configs.push_back(nullptr);

  Qnn_DeviceHandle_t device_handle = nullptr;
  ret = qnn.deviceCreate(log_handle, device_configs.data(), &device_handle);
  std::printf("deviceCreate ret=%lu handle=%p\n",
              static_cast<unsigned long>(ret), device_handle);

  if (backend_extensions) {
    std::printf("extensions.afterCreateDevice=%d\n",
                backend_extensions->afterCreateDevice());
  }
  if (device_handle) {
    Qnn_ErrorHandle_t free_ret = qnn.deviceFree(device_handle);
    std::printf("deviceFree ret=%lu\n", static_cast<unsigned long>(free_ret));
  }
  if (backend_extensions && destroy_backend_interface) {
    destroy_backend_interface(backend_extensions);
  }
  if (extensions_handle) dlclose(extensions_handle);
  if (backend_handle) {
    Qnn_ErrorHandle_t free_ret = qnn.backendFree(backend_handle);
    std::printf("backendFree ret=%lu\n", static_cast<unsigned long>(free_ret));
  }
  if (log_handle) {
    Qnn_ErrorHandle_t free_ret = qnn.logFree(log_handle);
    std::printf("logFree ret=%lu\n", static_cast<unsigned long>(free_ret));
  }

  return ret == QNN_SUCCESS ? 0 : 10;
}
