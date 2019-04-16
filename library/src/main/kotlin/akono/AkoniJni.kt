package akono;

class AkonoJni {
  external fun stringFromJNI(): String;

  companion object {
    init {
      System.loadLibrary("akono-jni")
    }
  }
}
