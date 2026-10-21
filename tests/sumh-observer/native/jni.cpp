#include "collector.hpp"
#include <jni.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::string from_java(JNIEnv *env, jstring input, size_t limit) {
  if (!input)
    throw std::invalid_argument("null string");
  jsize len = env->GetStringLength(input);
  if (static_cast<size_t>(len) > limit)
    throw std::invalid_argument("string exceeds byte limit");
  const jchar *chars = env->GetStringChars(input, nullptr);
  if (!chars)
    throw std::runtime_error("Java string allocation failed");
  std::string result;
  try {
    for (jsize i = 0; i < len; ++i) {
      unsigned cp = chars[i];
      if (cp >= 0xd800 && cp <= 0xdbff) {
        if (++i >= len || chars[i] < 0xdc00 || chars[i] > 0xdfff)
          throw std::invalid_argument("unpaired surrogate");
        cp = 0x10000 + ((cp - 0xd800) << 10) + (chars[i] - 0xdc00);
      } else if (cp >= 0xdc00 && cp <= 0xdfff)
        throw std::invalid_argument("unpaired surrogate");
      if (cp < 0x80)
        result += static_cast<char>(cp);
      else if (cp < 0x800) {
        result += static_cast<char>(0xc0 | (cp >> 6));
        result += static_cast<char>(0x80 | (cp & 63));
      } else if (cp < 0x10000) {
        result += static_cast<char>(0xe0 | (cp >> 12));
        result += static_cast<char>(0x80 | ((cp >> 6) & 63));
        result += static_cast<char>(0x80 | (cp & 63));
      } else {
        result += static_cast<char>(0xf0 | (cp >> 18));
        result += static_cast<char>(0x80 | ((cp >> 12) & 63));
        result += static_cast<char>(0x80 | ((cp >> 6) & 63));
        result += static_cast<char>(0x80 | (cp & 63));
      }
      if (result.size() > limit)
        throw std::invalid_argument("string exceeds byte limit");
    }
  } catch (...) {
    env->ReleaseStringChars(input, chars);
    throw;
  }
  env->ReleaseStringChars(input, chars);
  return result;
}
jstring to_java(JNIEnv *env, const std::string &input, size_t limit) {
  if (input.size() > limit)
    throw std::runtime_error("native result exceeds byte limit");
  std::vector<jchar> chars;
  for (size_t i = 0; i < input.size();) {
    unsigned first = static_cast<unsigned char>(input[i++]);
    unsigned cp;
    unsigned need;
    unsigned minimum;
    if (first < 0x80) {
      cp = first;
      need = 0;
      minimum = 0;
    } else if (first >= 0xc2 && first <= 0xdf) {
      cp = first & 31;
      need = 1;
      minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
      cp = first & 15;
      need = 2;
      minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
      cp = first & 7;
      need = 3;
      minimum = 0x10000;
    } else
      throw std::runtime_error("native result invalid UTF-8");
    if (need > input.size() - i)
      throw std::runtime_error("native result invalid UTF-8");
    for (unsigned j = 0; j < need; ++j) {
      unsigned c = static_cast<unsigned char>(input[i++]);
      if ((c & 0xc0) != 0x80)
        throw std::runtime_error("native result invalid UTF-8");
      cp = (cp << 6) | (c & 63);
    }
    if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
      throw std::runtime_error("native result invalid UTF-8");
    if (cp < 0x10000)
      chars.push_back(static_cast<jchar>(cp));
    else {
      cp -= 0x10000;
      chars.push_back(static_cast<jchar>(0xd800 | (cp >> 10)));
      chars.push_back(static_cast<jchar>(0xdc00 | (cp & 1023)));
    }
  }
  return env->NewString(chars.data(), static_cast<jsize>(chars.size()));
}
void fail(JNIEnv *env, const char *message) {
  if (env->ExceptionCheck())
    return;
  jclass type = env->FindClass("java/lang/IllegalArgumentException");
  if (type)
    env->ThrowNew(type, message);
}
} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_zying_zysu_sumhobservertest_NativeBridge_nCollect(JNIEnv *env, jclass,
                                                           jstring request,
                                                           jstring role,
                                                           jint fd) {
  try {
    return to_java(env,
                   sumh_observer::collect(from_java(env, request, 32768),
                                          from_java(env, role, 16), fd),
                   262144);
  } catch (const std::exception &e) {
    fail(env, e.what());
    return nullptr;
  } catch (...) {
    fail(env, "unknown native collector error");
    return nullptr;
  }
}
extern "C" JNIEXPORT jstring JNICALL
Java_com_zying_zysu_sumhobservertest_NativeBridge_nCreateFixture(
    JNIEnv *env, jclass, jstring files_dir, jstring run_id) {
  try {
    return to_java(
        env,
        sumh_observer::create_fixture(from_java(env, files_dir, 1024),
                                      from_java(env, run_id, 32)),
        32768);
  } catch (const std::exception &e) {
    fail(env, e.what());
    return nullptr;
  } catch (...) {
    fail(env, "unknown native fixture error");
    return nullptr;
  }
}
extern "C" JNIEXPORT void JNICALL
Java_com_zying_zysu_sumhobservertest_NativeBridge_nValidateRequest(
    JNIEnv *env, jclass, jstring request) {
  try {
    sumh_observer::validate_request(from_java(env, request, 32768));
  } catch (const std::exception &e) {
    fail(env, e.what());
  } catch (...) {
    fail(env, "unknown native request validation error");
  }
}
