package com.deviceagent

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.lang.reflect.Method
import java.nio.file.Files
import java.security.MessageDigest
import java.util.concurrent.atomic.AtomicInteger

class ResumableDownloadTest {
    @Test
    fun freshDownloadsForSha256AndMd5() {
        for (algo in algorithms()) {
            val body = "fresh-${algo.name}".toByteArray()
            TestHttpServer { request ->
                assertEquals("GET", request.method)
                HttpResponse(200, body)
            }.use { server ->
                val dest = File(tempDir(), "${algo.name}-fresh.apk")

                val ok = algo.invoke(server.url("/file.apk"), dest, digest(algo.name, body))

                assertTrue(ok)
                assertTrue(dest.exists())
                assertFalse(File(dest.absolutePath + ".part").exists())
                assertEquals(body.decodeToString(), dest.readText())
            }
        }
    }

    @Test
    fun resume206ForSha256AndMd5() {
        for (algo in algorithms()) {
            val full = "resume-${algo.name}-payload".toByteArray()
            val prefix = full.copyOfRange(0, 7)
            val suffix = full.copyOfRange(7, full.size)
            TestHttpServer { request ->
                if (request.method == "HEAD") {
                    HttpResponse(500)
                } else {
                    assertEquals("bytes=7-", request.headers["Range"])
                    HttpResponse(206, suffix, mapOf("Content-Range" to "bytes 7-${full.size - 1}/${full.size}"))
                }
            }.use { server ->
                val dest = File(tempDir(), "${algo.name}-resume.apk")
                File(dest.absolutePath + ".part").writeBytes(prefix)

                val ok = algo.invoke(server.url("/file.apk"), dest, digest(algo.name, full))

                assertTrue(ok)
                assertEquals(full.decodeToString(), dest.readText())
                val methods = server.requests.map { it.method }
                if (algo.name == "SHA-256") assertEquals(listOf("HEAD", "GET"), methods) else assertEquals(listOf("GET"), methods)
            }
        }
    }

    @Test
    fun range416RetriesFullForSha256AndMd5() {
        for (algo in algorithms()) {
            val body = "retry-${algo.name}".toByteArray()
            val getCount = AtomicInteger(0)
            TestHttpServer { request ->
                if (request.method == "HEAD") {
                    HttpResponse(500)
                } else if (getCount.getAndIncrement() == 0) {
                    assertEquals("bytes=32-", request.headers["Range"])
                    HttpResponse(416, headers = mapOf("Content-Range" to "bytes */${body.size}"))
                } else {
                    assertEquals(null, request.headers["Range"])
                    HttpResponse(200, body)
                }
            }.use { server ->
                val dest = File(tempDir(), "${algo.name}-retry.apk")
                File(dest.absolutePath + ".part").writeBytes(ByteArray(32) { 1 })

                val ok = algo.invoke(server.url("/file.apk"), dest, digest(algo.name, body))

                assertTrue(ok)
                assertEquals(body.decodeToString(), dest.readText())
                val methods = server.requests.map { it.method }
                if (algo.name == "SHA-256") assertEquals(listOf("HEAD", "GET", "GET"), methods) else assertEquals(listOf("GET", "GET"), methods)
            }
        }
    }

    private data class Algo(val name: String, val invoke: (String, File, String) -> Boolean)

    private fun algorithms(): List<Algo> {
        val svc = DeviceAgentService()
        val shaMethod = DeviceAgentService::class.java.getDeclaredMethod(
            "downloadFile",
            String::class.java,
            File::class.java,
            String::class.java,
            Function2::class.java
        ).apply { isAccessible = true }
        val md5Method = DeviceAgentService::class.java.getDeclaredMethod(
            "downloadFileMD5",
            String::class.java,
            File::class.java,
            String::class.java
        ).apply { isAccessible = true }
        return listOf(
            Algo("SHA-256") { url, dest, expected -> shaMethod.invoke(svc, url, dest, expected, null) as Boolean },
            Algo("MD5") { url, dest, expected -> md5Method.invoke(svc, url, dest, expected) as Boolean }
        )
    }

    private fun tempDir(): File = Files.createTempDirectory("resumable-download-test-").toFile().also { it.deleteOnExit() }

    private fun digest(algo: String, bytes: ByteArray): String {
        return MessageDigest.getInstance(algo).digest(bytes).joinToString("") { "%02x".format(it) }
    }
}
