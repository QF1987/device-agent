package com.deviceagent

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.BufferedReader
import java.io.ByteArrayOutputStream
import java.io.Closeable
import java.io.File
import java.io.InputStreamReader
import java.lang.reflect.Method
import java.net.ServerSocket
import java.net.Socket
import java.nio.file.Files
import java.security.MessageDigest
import java.util.Collections
import kotlin.concurrent.thread

class HeadProbeTest {
    private val downloadFile: Method = DeviceAgentService::class.java.getDeclaredMethod(
        "downloadFile",
        String::class.java,
        File::class.java,
        String::class.java,
        Function2::class.java
    ).apply { isAccessible = true }

    @Test
    fun shaMismatchDropsPartAndRetriesWithoutRange() {
        val full = "fresh server body".toByteArray()
        val expectedSha = sha256(full)
        TestHttpServer { request ->
            when (request.method) {
                "HEAD" -> Response(200, headers = mapOf("X-File-SHA256" to "deadbeef", "X-File-Size" to full.size.toString()))
                else -> {
                    assertEquals(null, request.headers["Range"])
                    Response(200, full)
                }
            }
        }.use { server ->
            val dest = File(tempDir(), "sha-change.apk")
            File(dest.absolutePath + ".part").writeText("stale prefix")

            val ok = invokeDownload(server.url("/file.apk"), dest, expectedSha)

            assertTrue(ok)
            assertEquals(2, server.requests.size)
            assertEquals("HEAD", server.requests[0].method)
            assertEquals("GET", server.requests[1].method)
            assertTrue(dest.exists())
            assertFalse(File(dest.absolutePath + ".part").exists())
            assertEquals(full.decodeToString(), dest.readText())
        }
    }

    @Test
    fun completePartSkipsGetAndRenamesAfterVerification() {
        val full = "already complete".toByteArray()
        val progressEvents = mutableListOf<Pair<Long, Long>>()
        TestHttpServer { request ->
            assertEquals("HEAD", request.method)
            Response(200, headers = mapOf("X-File-SHA256" to sha256(full), "X-File-Size" to full.size.toString()))
        }.use { server ->
            val dest = File(tempDir(), "complete.apk")
            File(dest.absolutePath + ".part").writeBytes(full)

            val ok = invokeDownload(server.url("/file.apk"), dest, sha256(full)) { abs, total ->
                progressEvents.add(abs to total)
            }

            assertTrue(ok)
            assertEquals(1, server.requests.size)
            assertEquals("HEAD", server.requests.single().method)
            assertTrue(dest.exists())
            assertFalse(File(dest.absolutePath + ".part").exists())
            assertEquals(full.decodeToString(), dest.readText())
            assertEquals(listOf(full.size.toLong() to full.size.toLong()), progressEvents)
        }
    }

    @Test
    fun headFailureFallsBackToRangeGet() {
        val full = "abcdefghijklmnopqrstuvwxyz".toByteArray()
        val prefix = full.copyOfRange(0, 8)
        val suffix = full.copyOfRange(8, full.size)
        TestHttpServer { request ->
            when (request.method) {
                "HEAD" -> Response(500)
                else -> {
                    assertEquals("bytes=8-", request.headers["Range"])
                    Response(206, suffix, mapOf("Content-Range" to "bytes 8-${full.size - 1}/${full.size}"))
                }
            }
        }.use { server ->
            val dest = File(tempDir(), "head-fail.apk")
            File(dest.absolutePath + ".part").writeBytes(prefix)

            val ok = invokeDownload(server.url("/file.apk"), dest, sha256(full))

            assertTrue(ok)
            assertEquals(listOf("HEAD", "GET"), server.requests.map { it.method })
            assertEquals(full.decodeToString(), dest.readText())
        }
    }

    @Test
    fun matchingHeadContinuesRangeGet() {
        val full = "range after head".toByteArray()
        val prefix = full.copyOfRange(0, 6)
        val suffix = full.copyOfRange(6, full.size)
        TestHttpServer { request ->
            when (request.method) {
                "HEAD" -> Response(200, headers = mapOf("X-File-SHA256" to sha256(full), "X-File-Size" to full.size.toString()))
                else -> {
                    assertEquals("bytes=6-", request.headers["Range"])
                    Response(206, suffix, mapOf("Content-Range" to "bytes 6-${full.size - 1}/${full.size}"))
                }
            }
        }.use { server ->
            val dest = File(tempDir(), "head-resume.apk")
            File(dest.absolutePath + ".part").writeBytes(prefix)

            val ok = invokeDownload(server.url("/file.apk"), dest, sha256(full))

            assertTrue(ok)
            assertEquals(listOf("HEAD", "GET"), server.requests.map { it.method })
            assertEquals(full.decodeToString(), dest.readText())
        }
    }

    private fun invokeDownload(
        url: String,
        dest: File,
        sha: String,
        progress: ((Long, Long) -> Unit)? = null
    ): Boolean {
        return downloadFile.invoke(DeviceAgentService(), url, dest, sha, progress) as Boolean
    }

    private fun tempDir(): File {
        return Files.createTempDirectory("head-probe-test-").toFile().also { it.deleteOnExit() }
    }

    private fun sha256(bytes: ByteArray): String {
        return MessageDigest.getInstance("SHA-256")
            .digest(bytes)
            .joinToString("") { "%02x".format(it) }
    }

    private data class Request(val method: String, val path: String, val headers: Map<String, String>)

    private data class Response(
        val code: Int,
        val body: ByteArray = ByteArray(0),
        val headers: Map<String, String> = emptyMap()
    )

    private class TestHttpServer(
        private val responder: (Request) -> Response
    ) : Closeable {
        private val server = ServerSocket(0)
        private var running = true
        val requests = Collections.synchronizedList(mutableListOf<Request>())

        private val worker = thread(start = true, isDaemon = true) {
            while (running) {
                try {
                    handle(server.accept())
                } catch (_: Exception) {
                    if (running) throw RuntimeException("HTTP test server failed")
                }
            }
        }

        fun url(path: String): String = "http://127.0.0.1:${server.localPort}$path"

        private fun handle(socket: Socket) {
            socket.use {
                val reader = BufferedReader(InputStreamReader(it.getInputStream()))
                val requestLine = reader.readLine() ?: return
                val parts = requestLine.split(" ")
                val method = parts.getOrElse(0) { "GET" }
                val path = parts.getOrElse(1) { "/" }
                val headers = linkedMapOf<String, String>()
                while (true) {
                    val line = reader.readLine() ?: break
                    if (line.isEmpty()) break
                    val split = line.indexOf(':')
                    if (split > 0) headers[line.substring(0, split)] = line.substring(split + 1).trim()
                }
                val request = Request(method, path, headers)
                requests.add(request)
                val response = responder(request)
                val out = ByteArrayOutputStream()
                val reason = when (response.code) {
                    200 -> "OK"
                    206 -> "Partial Content"
                    500 -> "Internal Server Error"
                    else -> "Status"
                }
                out.write("HTTP/1.1 ${response.code} $reason\r\n".toByteArray())
                out.write("Content-Length: ${response.body.size}\r\n".toByteArray())
                response.headers.forEach { (name, value) ->
                    out.write("$name: $value\r\n".toByteArray())
                }
                out.write("Connection: close\r\n\r\n".toByteArray())
                if (method != "HEAD") out.write(response.body)
                it.getOutputStream().write(out.toByteArray())
                it.getOutputStream().flush()
            }
        }

        override fun close() {
            running = false
            server.close()
            worker.join(1000)
        }
    }
}
