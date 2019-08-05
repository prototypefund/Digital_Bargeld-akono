package akono

import java.nio.ByteBuffer

typealias AkonoNativePointer = ByteBuffer

class AkonoJni {
    external fun stringFromJNI(): String

    private external fun evalJs(source: String, p: AkonoNativePointer): String

    private external fun destroyNative(b: AkonoNativePointer)
    private external fun initNative(nodeArgv: Array<out String>): AkonoNativePointer

    private external fun runNodeLoop(b: AkonoNativePointer)

    private external fun postMessageToNode(message: String, b: AkonoNativePointer)

    private external fun waitForMessageFromNode(b: AkonoNativePointer): String

    private var internalNativePointer: AkonoNativePointer

    fun evalJs(source: String): String = evalJs(source, internalNativePointer)

    @Override
    protected fun finalize() {
        destroyNative(internalNativePointer)
    }

    constructor(vararg nodeArgv: String) {
        internalNativePointer = initNative(nodeArgv)
    }

    companion object {
        init {
            System.loadLibrary("akono-jni")
        }
    }
}