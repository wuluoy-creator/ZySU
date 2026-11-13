#include "manager_client.hpp"
#include "sumhp/sumh_client.hpp"
#include <array>
#include <functional>
#include <jni.h>

namespace {
using zysu::manager::call;
using zysu::manager::Operation;
void throw_io(JNIEnv *env, const std::exception &error) {
  if (!env->ExceptionCheck())
    env->ThrowNew(env->FindClass("java/io/IOException"), error.what());
}
std::string bytes(JNIEnv *env, jbyteArray array) {
  if (!array)
    zysu::manager::fail("Missing data", EINVAL);
  const auto size = env->GetArrayLength(array);
  if (size > static_cast<jsize>(zysu::manager::max_payload))
    zysu::manager::fail("JNI input too large", EOVERFLOW);
  std::string value(static_cast<size_t>(size), '\0');
  env->GetByteArrayRegion(array, 0, size,
                          reinterpret_cast<jbyte *>(value.data()));
  if (env->ExceptionCheck())
    zysu::manager::fail("Read JNI input", EINVAL);
  return value;
}
jbyteArray result(JNIEnv *env, const std::function<std::string()> &action) {
  try {
    const auto output = action();
    auto *array = env->NewByteArray(static_cast<jsize>(output.size()));
    if (array)
      env->SetByteArrayRegion(array, 0, static_cast<jsize>(output.size()),
                              reinterpret_cast<const jbyte *>(output.data()));
    return array;
  } catch (const std::exception &error) {
    throw_io(env, error);
    return nullptr;
  }
}
void discard(JNIEnv *env, jbyteArray output) {
  if (output)
    env->DeleteLocalRef(output);
}
} // namespace

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_zying_zysu_Natives_sumhpRequest(JNIEnv *env, jobject /*unused*/,
                                         jbyteArray ksud, jbyteArray request) {
  return result(env, [&] {
    return call(Operation::Controller, {bytes(env, ksud), bytes(env, request)});
  });
}
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_zying_zysu_Natives_sumhKernelSnapshot(JNIEnv *env,
                                               jobject /*unused*/) {
  return result(env, [] { return call(Operation::Snapshot); });
}
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_zying_zysu_Natives_sumhRetryUserHide(JNIEnv *env, jobject /*unused*/,
                                              jbyteArray path) {
  return result(
      env, [&] { return call(Operation::RetryUserHide, {bytes(env, path)}); });
}
extern "C" JNIEXPORT jbyteArray JNICALL Java_com_zying_zysu_Natives_saveSuPath(
    JNIEnv *env, jobject /*unused*/, jbyteArray ksud, jbyteArray path) {
  return result(env, [&] {
    return call(Operation::SaveSuPath, {bytes(env, ksud), bytes(env, path)});
  });
}
extern "C" JNIEXPORT jboolean JNICALL
Java_com_zying_zysu_Natives_sumhIsInitialized(JNIEnv * /*unused*/,
                                              jobject /*unused*/) {
  return sumhp::sumh::is_available() ? JNI_TRUE : JNI_FALSE;
}
extern "C" JNIEXPORT jint JNICALL Java_com_zying_zysu_Natives_sumhRuntimeState(
    JNIEnv * /*unused*/, jobject /*unused*/) {
  return sumhp::sumh::enabled_state();
}
extern "C" JNIEXPORT jbyteArray JNICALL Java_com_zying_zysu_Natives_sumhReadLog(
    JNIEnv *env, jobject /*unused*/, jboolean kernel) {
  return result(env,
                [&] { return call(Operation::ReadLog, {kernel ? "1" : "0"}); });
}
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_zying_zysu_Natives_sumhStorageInfo(JNIEnv *env, jobject /*unused*/,
                                            jbyteArray path) {
  return result(
      env, [&] { return call(Operation::StorageInfo, {bytes(env, path)}); });
}
extern "C" JNIEXPORT void JNICALL
Java_com_zying_zysu_Natives_sumhClearLog(JNIEnv *env, jobject /*unused*/) {
  discard(env, result(env, [] { return call(Operation::ClearLog); }));
}
extern "C" JNIEXPORT void JNICALL
Java_com_zying_zysu_Natives_sumhClearMapsRules(JNIEnv *env,
                                               jobject /*unused*/) {
  discard(env, result(env, [] { return call(Operation::ClearMaps); }));
}
extern "C" JNIEXPORT void JNICALL Java_com_zying_zysu_Natives_sumhAddMapsRule(
    JNIEnv *env, jobject /*unused*/, jlongArray numbers, jbyteArray path) {
  discard(env, result(env, [&] {
            if (!numbers || env->GetArrayLength(numbers) != 4)
              zysu::manager::fail("Invalid Maps fields", EINVAL);
            std::array<jlong, 4> values{};
            env->GetLongArrayRegion(numbers, 0, 4, values.data());
            if (env->ExceptionCheck())
              zysu::manager::fail("Read Maps fields", EINVAL);
            std::vector<std::string> fields;
            fields.reserve(values.size() + 1);
            for (const auto value : values)
              fields.push_back(std::to_string(static_cast<uint64_t>(value)));
            fields.push_back(bytes(env, path));
            return call(Operation::AddMaps, fields);
          }));
}
