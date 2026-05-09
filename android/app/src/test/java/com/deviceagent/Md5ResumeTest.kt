package com.deviceagent

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.lang.reflect.Method
import java.nio.file.Files
import java.security.MessageDigest

class Md5ResumeTest {
    private val downloadFileMD5: Method = DeviceAgentService::class.java.getDeclaredMethod(
        "downloadFileMD5",
        String::class.java,
        File::class.java,
        String::class.java
    ).apply { isAccessible = true }

    @Test
    fun md5ResumeVerifiesAndDoesNotSendHead() {
        val full = "md5-resume-payload".toByteArray()
        val prefix = full.copyOfRange(0, 5)
        val suffix = full.copyOfRange(5, full.size)
        TestHttpServer { request ->
            assertEquals("GET", request.method)
            assertEquals("bytes=5-", request.headers["Range"])
            HttpResponse(206, suffix, mapOf("Content-Range" to "bytes 5-${full.size - 1}/${full.size}"))
        }.use { server ->
            val dest = File(tempDir(), "md5-resume.apk")
            File(dest.absolutePath + ".part").writeBytes(prefix)

            val ok = invokeDownload(server.url("/file.apk"), dest, md5(full))

            assertTrue(ok)
            assertEquals(listOf("GET"), server.requests.map { it.method })
            assertTrue(dest.exists())
            assertFalse(File(dest.absolutePath + ".part").exists())
            assertEquals(full.decodeToString(), dest.readText())
        }
    }

    @Test
    fun md5MismatchDeletesPartAndDest() {
        val body = "bad-md5".toByteArray()
        TestHttpServer { request ->
            assertEquals("GET", request.method)
            HttpResponse(200, body)
        }.use { server ->
            val dest = File(tempDir(), "md5-bad.apk")

            val ok = invokeDownload(server.url("/file.apk"), dest, "0000")

            assertFalse(ok)
            assertEquals(listOf("GET"), server.requests.map { it.method })
            assertFalse(dest.exists())
            assertFalse(File(dest.absolutePath + ".part").exists())
        }
    }

    private fun invokeDownload(url: String, dest: File, md5: String): Boolean {
        return downloadFileMD5.invoke(DeviceAgentService(), url, dest, md5) as Boolean
    }

    private fun tempDir(): File = Files.createTempDirectory("md5-resume-test-").toFile().also { it.deleteOnExit() }

    private fun md5(bytes: ByteArray): String {
        return MessageDigest.getInstance("MD5").digest(bytes).joinToString("") { "%02x".format(it) }
    }
}
