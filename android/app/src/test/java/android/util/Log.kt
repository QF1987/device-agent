package android.util

object Log {
    @Suppress("UNUSED_PARAMETER")
    @JvmStatic fun i(tag: String, msg: String): Int = 0
    @Suppress("UNUSED_PARAMETER")
    @JvmStatic fun w(tag: String, msg: String): Int = 0
    @Suppress("UNUSED_PARAMETER")
    @JvmStatic fun e(tag: String, msg: String): Int = 0
    @Suppress("UNUSED_PARAMETER")
    @JvmStatic fun e(tag: String, msg: String, tr: Throwable): Int = 0
}
