package com.deviceagent

import java.io.BufferedReader
import java.io.ByteArrayOutputStream
import java.io.Closeable
import java.io.InputStreamReader
import java.net.ServerSocket
import java.net.Socket
import java.util.Collections
import kotlin.concurrent.thread

internal data class HttpRequest(val method: String, val path: String, val headers: Map<String, String>)

internal data class HttpResponse(
    val code: Int,
    val body: ByteArray = ByteArray(0),
    val headers: Map<String, String> = emptyMap()
)

internal class TestHttpServer(
    private val responder: (HttpRequest) -> HttpResponse
) : Closeable {
    private val server = ServerSocket(0)
    private var running = true
    val requests = Collections.synchronizedList(mutableListOf<HttpRequest>())

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
            val request = HttpRequest(method, path, headers)
            requests.add(request)
            val response = responder(request)
            val out = ByteArrayOutputStream()
            val reason = when (response.code) {
                200 -> "OK"
                206 -> "Partial Content"
                416 -> "Range Not Satisfiable"
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
