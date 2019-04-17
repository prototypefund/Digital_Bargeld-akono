package akono;

class AkonoJni {
    external fun stringFromJNI(): String;

    external fun evalJs(source: String): String;

    companion object {
        init {
            System.loadLibrary("akono-jni")
        }
    }
}
