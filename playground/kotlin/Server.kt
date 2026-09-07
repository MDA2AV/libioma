// playground/kotlin - libioma driven from Kotlin/JVM through Panama (java.lang.foreign, JDK 22+).
//
// libioma does the I/O in C: one io_uring worker per core, a coroutine per connection. The
// handlers below are Kotlin functions turned into C function pointers (upcall stubs) and
// registered with ioma_route_ffi. libioma calls them on the worker's own thread stack (never on a
// coroutine stack, which the JVM could not run on), passing two flat structs it lays out itself:
// the request (ten 8-byte fields) and the reply (32 bytes). Build and run with ./run.sh.

import java.lang.foreign.Arena
import java.lang.foreign.FunctionDescriptor
import java.lang.foreign.Linker
import java.lang.foreign.MemorySegment
import java.lang.foreign.SymbolLookup
import java.lang.foreign.ValueLayout.ADDRESS
import java.lang.foreign.ValueLayout.JAVA_BYTE
import java.lang.foreign.ValueLayout.JAVA_INT
import java.lang.foreign.ValueLayout.JAVA_LONG
import java.lang.invoke.MethodHandle
import java.lang.invoke.MethodHandles
import java.lang.invoke.MethodType
import kotlin.system.exitProcess

/** Byte offsets of ioma_ffi_request (include/http.h): ten 8-byte fields, no padding. */
object Req {
    const val METHOD = 0L;  const val METHOD_LEN = 8L
    const val PATH = 16L;   const val PATH_LEN = 24L
    const val QUERY = 32L;  const val QUERY_LEN = 40L
    const val BODY = 48L;   const val BODY_LEN = 56L
    const val SCRATCH = 64L; const val SCRATCH_CAP = 72L
    const val SIZE = 88L
}

/** Byte offsets of ioma_ffi_response: status and close are ints, then three 8-byte fields. */
object Res {
    const val STATUS = 0L; const val CLOSE = 4L; const val CONTENT_TYPE = 8L
    const val BODY = 16L;  const val BODY_LEN = 24L
    const val SIZE = 32L
}

/** The binding: load the library, wrap the two calls we use, turn Kotlin functions into C pointers. */
object Ioma {
    private val linker = Linker.nativeLinker()
    private val keep = Arena.global()           // strings and stubs libioma keeps for its whole life
    private lateinit var routeFfi: MethodHandle
    private lateinit var runHandle: MethodHandle

    fun load(path: String) {
        val lib = SymbolLookup.libraryLookup(path, keep)
        routeFfi = linker.downcallHandle(lib.find("ioma_route_ffi").orElseThrow(),
            FunctionDescriptor.ofVoid(ADDRESS, ADDRESS, ADDRESS, ADDRESS))
        runHandle = linker.downcallHandle(lib.find("ioma_run").orElseThrow(),
            FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT))
    }

    /** A static (req, res, userdata) -> Unit function as a C function pointer. */
    fun stub(owner: Class<*>, name: String): MemorySegment {
        val mh = MethodHandles.lookup().findStatic(owner, name, MethodType.methodType(
            Void.TYPE, MemorySegment::class.java, MemorySegment::class.java, MemorySegment::class.java))
        return linker.upcallStub(mh, FunctionDescriptor.ofVoid(ADDRESS, ADDRESS, ADDRESS), keep)
    }

    fun route(method: String, path: String, handler: MemorySegment) {
        routeFfi.invokeWithArguments(keep.allocateFrom(method), keep.allocateFrom(path), handler, MemorySegment.NULL)
    }

    /** Blocks until SIGINT/SIGTERM. workers <= 0 means one per core. */
    fun run(workers: Int, port: Int): Int = runHandle.invokeWithArguments(workers, port) as Int
}

/** The endpoints. Allocation-free on the hot path: bytes are read from and written to C memory. */
object Handlers {
    private val hello = Arena.global().allocateFrom("hello from kotlin\n")   // NUL-terminated
    private val helloLen = hello.byteSize() - 1

    @JvmStatic
    fun hello(req: MemorySegment, res: MemorySegment, ud: MemorySegment) {
        val r = res.reinterpret(Res.SIZE)
        r.set(JAVA_INT, Res.STATUS, 200)
        r.set(ADDRESS, Res.BODY, hello)
        r.set(JAVA_LONG, Res.BODY_LEN, helloLen)
    }

    /** GET/POST /baseline11: the HttpArena baseline - sum the query values, plus the body on POST. */
    @JvmStatic
    fun baseline11(req: MemorySegment, res: MemorySegment, ud: MemorySegment) {
        val q = req.reinterpret(Req.SIZE)
        var sum = 0L
        val qlen = q.get(JAVA_LONG, Req.QUERY_LEN)
        if (qlen > 0) sum += sumQuery(q.get(ADDRESS, Req.QUERY).reinterpret(qlen), qlen)
        val blen = q.get(JAVA_LONG, Req.BODY_LEN)
        if (blen > 0) sum += leadingInt(q.get(ADDRESS, Req.BODY).reinterpret(blen), blen)

        val scratch = q.get(ADDRESS, Req.SCRATCH).reinterpret(q.get(JAVA_LONG, Req.SCRATCH_CAP))
        val n = putLong(scratch, if (sum < 0) 0 else sum)

        val r = res.reinterpret(Res.SIZE)
        r.set(JAVA_INT, Res.STATUS, 200)
        r.set(ADDRESS, Res.BODY, scratch)
        r.set(JAVA_LONG, Res.BODY_LEN, n)
    }

    private fun sumQuery(s: MemorySegment, n: Long): Long {
        var sum = 0L; var i = 0L
        while (i < n) {
            while (i < n && s.get(JAVA_BYTE, i) != '='.code.toByte()) i++
            if (i >= n) break
            i++
            var v = 0L; var any = false
            while (i < n && s.get(JAVA_BYTE, i) != '&'.code.toByte()) {
                val b = s.get(JAVA_BYTE, i).toInt()
                if (b in 48..57) { v = v * 10 + (b - 48); any = true }
                i++
            }
            if (any) sum += v
            if (i < n) i++
        }
        return sum
    }

    private fun leadingInt(s: MemorySegment, n: Long): Long {
        var v = 0L; var i = 0L
        while (i < n) { val b = s.get(JAVA_BYTE, i).toInt(); if (b !in 48..57) break; v = v * 10 + (b - 48); i++ }
        return v
    }

    /** Decimal digits of v into dst; returns how many. */
    private fun putLong(dst: MemorySegment, v: Long): Long {
        var x = v; var len = 0L
        do { len++; x /= 10 } while (x != 0L)
        x = v; var i = len - 1
        do { dst.set(JAVA_BYTE, i, (48 + (x % 10)).toByte()); x /= 10; i-- } while (x != 0L)
        return len
    }
}

fun main(args: Array<String>) {
    Ioma.load(args.getOrElse(0) { "../../libioma.so" })
    Ioma.route("GET", "/", Ioma.stub(Handlers::class.java, "hello"))
    val baseline = Ioma.stub(Handlers::class.java, "baseline11")
    Ioma.route("GET", "/baseline11", baseline)
    Ioma.route("POST", "/baseline11", baseline)
    val workers = System.getenv("IOMA_WORKERS")?.toIntOrNull() ?: 0
    val port = System.getenv("IOMA_PORT")?.toIntOrNull() ?: 8080
    println("kotlin handlers registered, handing the loop to libioma")
    exitProcess(Ioma.run(workers, port))
}
