package akono

import android.util.Base64
import android.util.Log
import java.lang.Exception
import java.nio.ByteBuffer
import java.util.concurrent.CountDownLatch
import java.util.concurrent.LinkedBlockingDeque
import kotlin.concurrent.thread

typealias AkonoNativePointer = ByteBuffer

private val TAG = "AkonoJni"

class AkonoJni(vararg nodeArgv: String) {
    private var messageHandler: MessageHandler? = null
    private val initializedLatch = CountDownLatch(1)

    private val workQueue = LinkedBlockingDeque<() -> Unit>()

    private external fun evalJs(source: String, p: AkonoNativePointer): String
    private external fun runNode(p: AkonoNativePointer)

    private external fun makeCallbackNative(source: String, p: AkonoNativePointer)
    private external fun putModuleCodeNative(key: String, source: String)

    private external fun destroyNative(b: AkonoNativePointer)
    private external fun initNative(nodeArgv: Array<out String>): AkonoNativePointer
    private external fun notifyNative(b: AkonoNativePointer)

    private lateinit var internalNativePointer: AkonoNativePointer

    private val jniThread: Thread

    private var stopped = false

    /**
     * Schedule a block do be executed in the node thread.
     */
    private fun scheduleNodeThread(b: () -> Unit) {
        initializedLatch.await()
        workQueue.put(b)
        notifyNative()
    }

    /**
     * Called by node/v8 from its thread.
     */
    private fun internalOnNotify(payload: String) {
        messageHandler?.handleMessage(payload)
    }


    fun notifyNative() {
        initializedLatch.await()
        notifyNative(internalNativePointer)
    }

    /**
     * Schedule Node.JS to be run.
     */
    fun evalSimpleJs(source: String): String {
        val latch = CountDownLatch(1)
        var result: String? = null
        scheduleNodeThread {
            result = evalJs(source, internalNativePointer)
            latch.countDown()
        }
        latch.await()
        return result ?: throw Exception("invariant failed")
    }

    fun evalNodeCode(source: String) {
        scheduleNodeThread {
            makeCallbackNative(source, internalNativePointer)
        }
    }

    /**
     * Send a message to node, calling global.__akono_onMessage.
     */
    fun sendMessage(message: String) {
        val encoded = Base64.encodeToString(message.toByteArray(), Base64.NO_WRAP)
        val source = """
            if (global.__akono_onMessage) {
              const msg = (new Buffer('$encoded', 'base64')).toString('ascii');
              global.__akono_onMessage(msg);
            } else {
                console.log("WARN: no __akono_onMessage defined");
            }
        """.trimIndent()
        evalNodeCode(source)
    }

    fun waitStopped(): Unit {
        Log.i(TAG, "waiting for stop")
        scheduleNodeThread {
            stopped = true
        }
        jniThread.join()
        return
    }

    fun putModuleCode(modName: String, code: String) {
        Log.v(TAG, "putting module code (kotlin)")
        putModuleCodeNative(modName, code)
    }

    /**
     * Register a message handler that is called when the JavaScript code
     * running in [runNodeJs] calls __akono_sendMessage
     *
     * Does not block.
     */
    fun setMessageHandler(handler: MessageHandler) {
        this.messageHandler = handler
    }


    @Override
    protected fun finalize() {
        destroyNative(internalNativePointer)
    }

    init {
        jniThread = thread {
            internalNativePointer = initNative(nodeArgv)
            initializedLatch.countDown()
            while (true) {
                runNode(internalNativePointer)
                while (true) {
                    val w = workQueue.poll() ?: break
                    w()
                }
                if (stopped) {
                    break
                }
            }
        }
    }

    companion object {
        init {
            System.loadLibrary("akono-jni")
        }
    }

    interface MessageHandler {
        fun handleMessage(message: String)
    }
}
