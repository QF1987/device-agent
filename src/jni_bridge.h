// jni_bridge.h - JNI bridge between android_executor.cc and jni_wrapper.cpp
// Both files share the same JVM state.

#ifndef JNI_BRIDGE_H
#define JNI_BRIDGE_H

#include <jni.h>
#include <string>

// These are defined in android_executor.cc (extern so they can be shared)
extern JavaVM* g_jvm;
extern jobject g_java_service;

// getJNIEnv() is also in android_executor.cc
JNIEnv* getJNIEnv();

// showToast() is static in android_executor.cc but we need it in jni_wrapper too
// We just define our own android_show_toast in jni_wrapper.cpp using the shared state

#endif  // JNI_BRIDGE_H
