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
    fun myAkonoTest() {
        val ajni: AkonoJni = AkonoJni()
        assertEquals("foo", ajni.stringFromJNI())
    }
}
