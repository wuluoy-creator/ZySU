#include "manager_client.hpp"
#include <jni.h>

namespace {
int run(JNIEnv *env, zysu::manager::Operation operation,
        jstring source = nullptr) {
  const char *path = nullptr;
  try {
    std::vector<std::string> fields;
    if (operation != zysu::manager::Operation::RepairLinks) {
      if (!source)
        return 0;
      path = env->GetStringUTFChars(source, nullptr);
      if (!path)
        return 0;
      fields.emplace_back(path);
      env->ReleaseStringUTFChars(source, path);
      path = nullptr;
    }
    const auto value = zysu::manager::call(operation, fields);
    if (value == "1")
      return 1;
    if (value == "2" && operation == zysu::manager::Operation::Verify)
      return 2;
  } catch (const std::exception &) {
    if (path)
      env->ReleaseStringUTFChars(source, path);
    // Existing API distinguishes unavailable from a verified mismatch.
  }
  return 0;
}
} // namespace
extern "C" JNIEXPORT jint JNICALL Java_com_zying_zysu_Natives_verifyKsudDaemon(
    JNIEnv *env, jobject /*unused*/, jstring source) {
  return run(env, zysu::manager::Operation::Verify, source);
}
extern "C" JNIEXPORT jboolean JNICALL
Java_com_zying_zysu_Natives_installKsudDaemon(JNIEnv *env, jobject /*unused*/,
                                              jstring source) {
  return run(env, zysu::manager::Operation::Install, source) == 1;
}
extern "C" JNIEXPORT jboolean JNICALL
Java_com_zying_zysu_Natives_ensureKsudToolLinks(JNIEnv *env,
                                                jobject /*unused*/) {
  return run(env, zysu::manager::Operation::RepairLinks) == 1;
}
