val kotlin_version by extra("1.3.21")

buildscript {
    var kotlin_version: String by extra
    kotlin_version = "1.3.30"
    repositories {
        google()
        jcenter()
    }
    dependencies {
        classpath("com.android.tools.build:gradle:3.3.2")
        classpath(kotlin("gradle-plugin", version = "1.3.21"))
    }
}

allprojects {
    repositories {
        google()
        jcenter()
    }

}
