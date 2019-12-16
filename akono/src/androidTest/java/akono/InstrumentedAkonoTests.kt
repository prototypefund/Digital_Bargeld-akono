package akono.test;

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.runner.RunWith
import org.junit.Test
import androidx.test.filters.LargeTest
import org.junit.Assert.assertEquals
import akono.AkonoJni
import android.util.Log
import java.util.concurrent.LinkedBlockingDeque


class SyncMessageHandler : AkonoJni.MessageHandler {
    private val messageQueue = LinkedBlockingDeque<String>()
    override fun handleMessage(message: String) {
        messageQueue.put(message)
    }

    fun waitForMessage(): String {
        return messageQueue.take()
    }
}

// @RunWith is required only if you use a mix of JUnit3 and JUnit4.
@RunWith(AndroidJUnit4::class)
@LargeTest
class InstrumentedAkonoTestOne {
    @Test
    fun myJsTest() {
        val ajni: AkonoJni = AkonoJni()
        ajni.putModuleCode("a", "function foo() {}")
        assertEquals("2", ajni.evalSimpleJs("1+1"))
        assertEquals("36", ajni.evalSimpleJs("6*6"))
        assertEquals("42", ajni.evalSimpleJs("(()=>{let x = 42; return x;})()"))
        assertEquals("undefined", ajni.evalSimpleJs("const myVal = 42"))
        assertEquals("43", ajni.evalSimpleJs("myVal + 1"))

        val myHandler = SyncMessageHandler()
        ajni.setMessageHandler(myHandler)
        ajni.evalNodeCode("console.log('hi from the test case')")
        // Tell the message handler to just ping back messages to us
        ajni.evalNodeCode("global.__akono_onMessage = (x) => { global.__akono_sendMessage(x); }")
        val sentMessage = "Hello AKONO!!"
        ajni.sendMessage(sentMessage)
        val receivedMessage = myHandler.waitForMessage()
        assertEquals(sentMessage, receivedMessage)
        Log.i("myapp", "test case received message: $receivedMessage")

        ajni.evalNodeCode("require('akono');")
        ajni.evalNodeCode("a = require('a');")
        //ajni.evalNodeCode("a.foo()")

        //val msg2 = myHandler.waitForMessage()

        //assertEquals("hello42", msg2)

        ajni.waitStopped()
    }
}
