package akono.test;

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.runner.RunWith
import org.junit.Test
import androidx.test.filters.SmallTest
import androidx.test.filters.LargeTest
import org.junit.Assert.assertTrue
import org.junit.Assert.assertEquals
import akono.AkonoJni

// @RunWith is required only if you use a mix of JUnit3 and JUnit4.
@RunWith(AndroidJUnit4::class)
@LargeTest
public class InstrumentedAkonoTestOne {
    @Test
    fun myJsTest() {
        val ajni: AkonoJni = AkonoJni()
        assertEquals("2", ajni.evalJs("1+1"))
        assertEquals("36", ajni.evalJs("6*6"))
        assertEquals("42", ajni.evalJs("(()=>{let x = 42; return x;})()"))
        //assertEquals(null, ajni.evalJs("throw Error('hello exc')"))
        //assertEquals(null, ajni.evalJs("undefinedX + undefinedY"))
        //assertEquals("123", ajni.evalJs("console.log('hello world'); 123;"))
        //assertEquals("123", ajni.evalJs("require"))

        assertEquals("undefined", ajni.evalJs("const myVal = 42"))
        assertEquals("43", ajni.evalJs("myVal + 1"))
    }
}
