/*
 * Placeholder source for the "mpg" (libmpg.so) target of the mafon native build.
 *
 * The real MP3Decoder JNI wrapper is provided by the Lead as
 *     F:\harness1\mafon-work\native\jni\mpg_jni.c
 * (package nobleworks.libmpg, class MP3Decoder).
 *
 * native/CMakeLists.txt automatically prefers jni/mpg_jni.c whenever that file
 * exists. If it does not exist yet, the "mpg" target is still defined and built
 * from this placeholder so that the whole build pipeline (compile + link against
 * the static libmpg123.a) is exercised end to end. The placeholder is NOT
 * staged into out\<abi>\ - only a real mpg_jni.c build is installed there.
 */
#include <jni.h>

/* Never referenced by the app; keeps the placeholder obviously fake. */
int mafon_mpg_jni_placeholder = 1;

/* Minimal valid entry point so the placeholder can be dlopen()ed safely. */
JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)vm;
    (void)reserved;
    return JNI_VERSION_1_6;
}

JNIEXPORT jstring JNICALL
Java_mafon_placeholder_Placeholder_name(JNIEnv *env, jclass clazz)
{
    (void)clazz;
    return (*env)->NewStringUTF(env, "mpg_jni placeholder - rebuild with jni/mpg_jni.c");
}
