#include <android/log.h>
#include <jni.h>

#include <cstddef>

namespace {
constexpr const char* kLogTag = "PokemonStadium2Recomp";
}
extern "C" __attribute__((visibility("default"))) int ps2_android_probe() {
    const bool isArm64 = sizeof(void*) == 8;
    __android_log_print(
        ANDROID_LOG_INFO,
        kLogTag,
        "Android ARM64 build probe loaded: ptr=%zu, api=%d",
        sizeof(void*),
        __ANDROID_API__);
    return isArm64 ? 1 : 0;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_pokemonstadium2recomp_probe_MainActivity_nativeRunProbe(JNIEnv*, jobject) {
    return ps2_android_probe();
}
