#include <android/log.h>
#include <dlfcn.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <adrenotools/driver.h>
#include <volk/volk.h>

namespace {

constexpr const char* kLogTag = "PSR-GpuDriver";
constexpr const char* kDriverRoot = "PSR_GPU_DRIVER_ROOT";
constexpr const char* kDriverTmp = "PSR_GPU_DRIVER_TMP";
constexpr const char* kNativeLibraryDir = "PSR_NATIVE_LIBRARY_DIR";
constexpr const char* kDriverDir = "PSR_GPU_DRIVER_DIR";
constexpr const char* kDriverSoname = "PSR_GPU_DRIVER_SONAME";
constexpr const char* kDriverName = "PSR_GPU_DRIVER_NAME";

void* g_customVulkanHandle = nullptr;

VkResult use_system_driver(const std::string& reason) {
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
        "Using system Vulkan driver: %s", reason.c_str());
    return volkInitialize();
}

bool is_managed_driver_path(const std::filesystem::path& driverDir) {
    const char* rootValue = std::getenv(kDriverRoot);
    if (rootValue == nullptr || rootValue[0] == '\0') {
        return false;
    }

    std::error_code error;
    const auto importsDir = std::filesystem::weakly_canonical(
        std::filesystem::path(rootValue) / "imports", error);
    if (error) {
        return false;
    }
    const auto canonicalDriverDir = std::filesystem::weakly_canonical(driverDir, error);
    if (error || canonicalDriverDir == importsDir) {
        return false;
    }

    const auto relative = canonicalDriverDir.lexically_relative(importsDir);
    return !relative.empty() && *relative.begin() != "..";
}

} // namespace

namespace RT64 {

VkResult psr_android_initialize_vulkan_loader() {
    const char* driverDirValue = std::getenv(kDriverDir);
    const char* driverSonameValue = std::getenv(kDriverSoname);
    if (driverDirValue == nullptr || driverDirValue[0] == '\0' ||
        driverSonameValue == nullptr || driverSonameValue[0] == '\0') {
        return use_system_driver("no custom driver selected");
    }

    const std::filesystem::path driverDir(driverDirValue);
    const std::filesystem::path soname(driverSonameValue);
    if (soname.empty() || soname != soname.filename()) {
        return use_system_driver("invalid custom driver filename");
    }
    if (!is_managed_driver_path(driverDir)) {
        return use_system_driver("custom driver is outside app-managed storage");
    }

    std::error_code error;
    const auto driverLibrary = driverDir / soname;
    if (!std::filesystem::is_regular_file(driverLibrary, error) || error) {
        return use_system_driver("custom driver library is missing");
    }

    const char* tmpDir = std::getenv(kDriverTmp);
    const char* nativeLibraryDir = std::getenv(kNativeLibraryDir);
    if (tmpDir == nullptr || tmpDir[0] == '\0' ||
        nativeLibraryDir == nullptr || nativeLibraryDir[0] == '\0') {
        return use_system_driver("Android driver-loader directories are unavailable");
    }

    std::string driverDirWithSeparator = driverDir.string();
    if (driverDirWithSeparator.back() != '/') {
        driverDirWithSeparator.push_back('/');
    }

    const char* displayName = std::getenv(kDriverName);
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
        "Loading custom Vulkan driver name=%s dir=%s soname=%s",
        displayName != nullptr && displayName[0] != '\0' ? displayName : "Custom driver",
        driverDirValue, driverSonameValue);

    dlerror();
    g_customVulkanHandle = adrenotools_open_libvulkan(
        RTLD_NOW | RTLD_LOCAL,
        ADRENOTOOLS_DRIVER_CUSTOM,
        tmpDir,
        nativeLibraryDir,
        driverDirWithSeparator.c_str(),
        driverSonameValue,
        nullptr,
        nullptr);
    if (g_customVulkanHandle == nullptr) {
        const char* loaderError = dlerror();
        return use_system_driver(loaderError != nullptr
            ? std::string("custom driver load failed: ") + loaderError
            : "custom driver load failed");
    }

    dlerror();
    auto getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(g_customVulkanHandle, "vkGetInstanceProcAddr"));
    const char* symbolError = dlerror();
    if (getInstanceProcAddr == nullptr || symbolError != nullptr) {
        dlclose(g_customVulkanHandle);
        g_customVulkanHandle = nullptr;
        return use_system_driver(symbolError != nullptr
            ? std::string("custom driver has no vkGetInstanceProcAddr: ") + symbolError
            : "custom driver has no vkGetInstanceProcAddr");
    }

    volkInitializeCustom(getInstanceProcAddr);
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
        "Custom Vulkan interception armed: %s",
        displayName != nullptr && displayName[0] != '\0' ? displayName : driverSonameValue);
    return VK_SUCCESS;
}

void psr_android_report_vulkan_driver_status(const char* deviceName, uint32_t driverVersion) {
    const char* driverDirValue = std::getenv(kDriverDir);
    const char* driverSonameValue = std::getenv(kDriverSoname);
    if (driverDirValue == nullptr || driverDirValue[0] == '\0' ||
        driverSonameValue == nullptr || driverSonameValue[0] == '\0') {
        return;
    }

    bool customDriverMapped = false;
    std::ifstream maps("/proc/self/maps");
    for (std::string line; std::getline(maps, line);) {
        if (line.find(driverSonameValue) != std::string::npos) {
            customDriverMapped = true;
            break;
        }
    }

    const char* displayName = std::getenv(kDriverName);
    if (customDriverMapped) {
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
            "Custom Vulkan driver active: %s device=%s driverVersion=0x%08x",
            displayName != nullptr && displayName[0] != '\0' ? displayName : driverSonameValue,
            deviceName != nullptr ? deviceName : "unknown", driverVersion);
    } else {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
            "Custom Vulkan driver failed or fell back to system: selected=%s device=%s driverVersion=0x%08x",
            displayName != nullptr && displayName[0] != '\0' ? displayName : driverSonameValue,
            deviceName != nullptr ? deviceName : "unknown", driverVersion);
    }
}

} // namespace RT64
