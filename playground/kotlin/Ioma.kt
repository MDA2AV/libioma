// Ioma.kt - a small Kotlin binding over libioma's foreign-handler ABI (include/http.h).
//
// One C-callable dispatcher serves every route: libioma passes the route's index back as userdata
// and the dispatcher invokes the matching Kotlin lambda. It reuses one Request and one Reply per
// worker thread, so a request allocates nothing on the JVM side. Handlers run on the worker's own
// thread stack and must not block.

import java.lang.foreign.Arena
import java.lang.foreign.FunctionDescriptor
import java.lang.foreign.Linker
import java.lang.foreign.MemorySegment
import java.lang.foreign.SymbolLookup
import java.lang.foreign.ValueLayout.ADDRESS
import java.lang.foreign.ValueLayout.JAVA_BYTE
import java.lang.foreign.ValueLayout.JAVA_INT
import java.lang.foreign.ValueLayout.JAVA_LONG
import java.lang.invoke.MethodHandles
import java.lang.invoke.MethodType

typealias Handler = (Request, Reply) -> Unit

/** A request: zero-copy views into libioma's read buffer, valid only while the handler runs. */
class Request internal constructor() {
    internal var raw: MemorySegment = MemorySegment.NULL
    val method: MemorySegment get() = view(0)
    val path: MemorySegment get() = view(16)
    val query: MemorySegment get() = view(32)
    val body: MemorySegment get() = view(48)
    private fun view(off: Long) = raw.get(ADDRESS, off).reinterpret(raw.get(JAVA_LONG, off + 8))
}

/** The reply. Bodies are written into libioma's per-request scratch buffer; status defaults to 200. */
class Reply internal constructor() {
    internal var raw: MemorySegment = MemorySegment.NULL
    internal var scratch: MemorySegment = MemorySegment.NULL
    var status: Int
        get() = raw.get(JAVA_INT, 0)
        set(v) = raw.set(JAVA_INT, 0, v)

    fun text(s: String) {           // ASCII
        var i = 0L
        for (ch in s) scratch.set(JAVA_BYTE, i++, ch.code.toByte())
        body(i)
    }
    fun text(n: Long) = body(scratch.putLong(n))
    private fun body(len: Long) { raw.set(ADDRESS, 16, scratch); raw.set(JAVA_LONG, 24, len) }
}

class Ioma(libPath: String) {
    private val linker = Linker.nativeLinker()
    private val arena = Arena.global()                   // libioma keeps these pointers for its whole life
    private val lib = SymbolLookup.libraryLookup(libPath, arena)
    private val routeFfi = linker.downcallHandle(lib.find("ioma_route_ffi").orElseThrow(),
        FunctionDescriptor.ofVoid(ADDRESS, ADDRESS, ADDRESS, ADDRESS))
    private val runFn = linker.downcallHandle(lib.find("ioma_run").orElseThrow(),
        FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT))
    private val handlers = ArrayList<Handler>()
    private val request = ThreadLocal.withInitial { Request() }
    private val reply = ThreadLocal.withInitial { Reply() }
    private val dispatcher: MemorySegment = linker.upcallStub(
        MethodHandles.lookup().findVirtual(Ioma::class.java, "dispatch", MethodType.methodType(
            Void.TYPE, MemorySegment::class.java, MemorySegment::class.java, MemorySegment::class.java)).bindTo(this),
        FunctionDescriptor.ofVoid(ADDRESS, ADDRESS, ADDRESS), arena)

    fun route(method: String, path: String, handler: Handler) {
        handlers += handler
        routeFfi.invokeWithArguments(arena.allocateFrom(method), arena.allocateFrom(path), dispatcher,
            MemorySegment.ofAddress((handlers.size - 1).toLong()))
    }
    fun get(path: String, handler: Handler) = route("GET", path, handler)
    fun post(path: String, handler: Handler) = route("POST", path, handler)

    /** Blocks until SIGINT/SIGTERM. workers <= 0 means one per core. */
    fun run(workers: Int = 0, port: Int = 8080): Int = runFn.invokeWithArguments(workers, port) as Int

    /** libioma calls this on a worker's thread stack for every foreign route. */
    fun dispatch(req: MemorySegment, res: MemorySegment, userdata: MemorySegment) {
        val r = request.get()
        r.raw = req.reinterpret(88)
        val y = reply.get()
        y.raw = res.reinterpret(32)
        y.scratch = r.raw.get(ADDRESS, 64).reinterpret(r.raw.get(JAVA_LONG, 72))
        handlers[userdata.address().toInt()](r, y)
    }
}

fun ioma(lib: String, routes: Ioma.() -> Unit): Ioma = Ioma(lib).apply(routes)

// ── byte helpers, so handlers never turn C memory into Strings ────────────────────────────────

/** Sum of the integer values in "a=13&b=42". */
fun MemorySegment.sumOfValues(): Long {
    val n = byteSize(); var sum = 0L; var i = 0L
    while (i < n) {
        while (i < n && get(JAVA_BYTE, i) != '='.code.toByte()) i++
        if (i >= n) break
        i++
        var v = 0L; var any = false
        while (i < n && get(JAVA_BYTE, i) != '&'.code.toByte()) {
            val d = get(JAVA_BYTE, i) - 48
            if (d in 0..9) { v = v * 10 + d; any = true }
            i++
        }
        if (any) sum += v
        if (i < n) i++
    }
    return sum
}

/** The integer a body like "20" starts with. */
fun MemorySegment.leadingInt(): Long {
    var v = 0L; var i = 0L
    while (i < byteSize()) { val d = get(JAVA_BYTE, i) - 48; if (d !in 0..9) break; v = v * 10 + d; i++ }
    return v
}

/** Decimal digits of v at the start of this segment; returns how many. */
internal fun MemorySegment.putLong(v: Long): Long {
    var x = if (v < 0) 0 else v; var len = 0L
    do { len++; x /= 10 } while (x != 0L)
    x = if (v < 0) 0 else v; var i = len - 1
    do { set(JAVA_BYTE, i--, (48 + x % 10).toByte()); x /= 10 } while (x != 0L)
    return len
}
