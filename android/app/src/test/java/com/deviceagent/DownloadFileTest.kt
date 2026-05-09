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
import java.util.concurrent.atomic.AtomicInteger
import kotlin.concurrent.thread

class DownloadFileTest {
    private val downloadFile: Method = DeviceAgentService::class.java.getDeclaredMethod(
        "downloadFile",
        String::class.java,
        File::class.java,
        String::class.java,
        Function2::class.java
    ).apply { isAccessible = true }

    @Test
    fun fresh200DownloadsToDestAndRemovesPart() {
        val body = "fresh download body".toByteArray()
        TestHttpServer { Response(200, body) }.use { server ->
            val dir = tempDir()
            val dest = File(dir, "fresh.apk")

            val ok = invokeDownload(server.url("/file.apk"), dest, sha256(body))

            assertTrue(ok)
            assertTrue(dest.exists())
            assertFalse(File(dest.absolutePath + ".part").exists())
            assertEquals(body.decodeToString(), dest.readText())
            assertEquals(null, server.requests.single().headers["Range"])
        }
    }

    @Test
    fun resume206AppendsFromExistingPart() {
        val full = "abcdefghijklmnopqrstuvwxyz".toByteArray()
        val prefix = full.copyOfRange(0, 10)
        val suffix = full.copyOfRange(10, full.size)
        TestHttpServer { request ->
            if (request.method == "HEAD") {
                Response(500)
            } else {
                assertEquals("bytes=10-", request.headers["Range"])
                Response(
                    206,
                    suffix,
                    mapOf("Content-Range" to "bytes 10-${full.size - 1}/${full.size}")
                )
            }
        }.use { server ->
            val dir = tempDir()
            val dest = File(dir, "resume.apk")
            File(dest.absolutePath + ".part").writeBytes(prefix)

            val ok = invokeDownload(server.url("/file.apk"), dest, sha256(full))

            assertTrue(ok)
            assertTrue(dest.exists())
            assertFalse(File(dest.absolutePath + ".part").exists())
            assertEquals(full.decodeToString(), dest.readText())
            assertEquals(listOf("HEAD", "GET"), server.requests.map { it.method })
            assertEquals("bytes=10-", server.requests.last().headers["Range"])
        }
    }

    @Test
    fun range416DropsPartAndRetriesFullDownload() {
        val full = "short body".toByteArray()
        val calls = AtomicInteger(0)
        TestHttpServer { request ->
            if (request.method == "HEAD") {
                Response(500)
            } else if (calls.getAndIncrement() == 0) {
                assertEquals("bytes=32-", request.headers["Range"])
                Response(416, ByteArray(0), mapOf("Content-Range" to "bytes */${full.size}"))
            } else {
                assertEquals(null, request.headers["Range"])
                Response(200, full)
            }
        }.use { server ->
            val dir = tempDir()
            val dest = File(dir, "retry.apk")
            File(dest.absolutePath + ".part").writeBytes(ByteArray(32) { 1 })

            val ok = invokeDownload(server.url("/file.apk"), dest, sha256(full))

            assertTrue(ok)
            assertEquals(3, server.requests.size)
            assertEquals(listOf("HEAD", "GET", "GET"), server.requests.map { it.method })
            assertTrue(dest.exists())
            assertFalse(File(dest.absolutePath + ".part").exists())
            assertEquals(full.decodeToString(), dest.readText())
        }
    }

    @Test
    fun shaMismatchDeletesPartAndDest() {
        val body = "tampered body".toByteArray()
        TestHttpServer { Response(200, body) }.use { server ->
            val dir = tempDir()
            val dest = File(dir, "bad.apk")

            val ok = invokeDownload(server.url("/file.apk"), dest, "0000")

            assertFalse(ok)
            assertFalse(dest.exists())
            assertFalse(File(dest.absolutePath + ".part").exists())
        }
    }

    private fun invokeDownload(url: String, dest: File, sha: String): Boolean {
        return downloadFile.invoke(DeviceAgentService(), url, dest, sha, null) as Boolean
    }

    private fun tempDir(): File {
        return Files.createTempDirectory("download-file-test-").toFile().also { it.deleteOnExit() }
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
        private val running = AtomicInteger(1)
        val requests = Collections.synchronizedList(mutableListOf<Request>())

        private val worker = thread(start = true, isDaemon = true) {
            while (running.get() == 1) {
                try {
                    handle(server.accept())
                } catch (_: Exception) {
                    if (running.get() == 1) throw RuntimeException("HTTP test server failed")
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
                    if (split > 0) {
                        headers[line.substring(0, split)] = line.substring(split + 1).trim()
                    }
                }
                val request = Request(method, path, headers)
                requests.add(request)
                val response = responder(request)
                val out = ByteArrayOutputStream()
                val reason = when (response.code) {
                    200 -> "OK"
                    206 -> "Partial Content"
                    416 -> "Range Not Satisfiable"
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
            running.set(0)
            server.close()
            worker.join(1000)
        }
    }
}
