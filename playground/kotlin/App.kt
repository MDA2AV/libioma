// App.kt - the example: the endpoints in Kotlin, the I/O in C. ./run.sh builds and runs it.

import kotlin.system.exitProcess

fun main(args: Array<String>) {
    val server = ioma(lib = args.getOrElse(0) { "../../libioma.so" }) {
        get("/") { _, reply -> reply.text("hello from kotlin\n") }
        get("/baseline11") { req, reply -> reply.text(req.query.sumOfValues()) }
        post("/baseline11") { req, reply -> reply.text(req.query.sumOfValues() + req.body.leadingInt()) }
    }
    println("kotlin handlers registered, handing the loop to libioma")
    exitProcess(server.run(workers = System.getenv("IOMA_WORKERS")?.toIntOrNull() ?: 0,
                           port = System.getenv("IOMA_PORT")?.toIntOrNull() ?: 8080))
}
