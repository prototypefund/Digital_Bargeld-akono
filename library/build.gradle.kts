plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.android.extensions")
}
apply {
    plugin("kotlin-android")
    plugin("kotlin-android-extensions")
}

android {
    compileSdkVersion(28)
    defaultConfig {
        minSdkVersion(26)
        targetSdkVersion(28)
        versionCode = 1
        versionName = "1.0"
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        // Specifies the application ID for the test APK.
        testApplicationId = "akono.test"

        ndk {
          // Tells Gradle to build outputs for the following ABIs and package
          // them into your APK.
            abiFilters("armeabi-v7a");
        }

        //externalNativeBuild {
        //  cmake {
        //    
        //  }
        //}
    }
    useLibrary("android.test.runner")
    useLibrary("android.test.base")
    useLibrary("android.test.mock")

    externalNativeBuild {
        cmake {
            //path = File("src/main/cpp/CMakeLists.txt")
            path = file("src/main/cpp/CMakeLists.txt")
        }
    }

    // Work around a bug in the android plugin.
    // Without this extra source set, test cases written in Kotlin are
    // compiled but not executed.
    sourceSets {
      named("androidTest") {
        java.srcDir("src/androidTest/kotlin")
      }
      //jniLibs.srcDirs(FIXME)
    }
}

val kotlin_version: String by rootProject.extra

repositories {
  jcenter()
}

dependencies {
    //implementation("org.jetbrains.kotlin:kotlin-stdlib-jdk8:1.3.20")
    //implementation(kotlin("stdlib"))

    // Use the Kotlin test library.
    testImplementation("org.jetbrains.kotlin:kotlin-test:$kotlin_version")

    // Use the Kotlin JUnit integration.
    testImplementation("org.jetbrains.kotlin:kotlin-test-junit:$kotlin_version")

    androidTestImplementation("androidx.test:core:1.0.0")
    androidTestImplementation("androidx.test:runner:1.1.1")
    androidTestImplementation("androidx.test:rules:1.1.1")

    // Assertions
    androidTestImplementation("androidx.test.ext:junit:1.0.0")
    androidTestImplementation("androidx.test.ext:truth:1.0.0")
    androidTestImplementation("com.google.truth:truth:0.42")

    // Use the Kotlin test library.
    androidTestImplementation("org.jetbrains.kotlin:kotlin-test:$kotlin_version")

    // Use the Kotlin JUnit integration.
    androidTestImplementation("org.jetbrains.kotlin:kotlin-test-junit:$kotlin_version")
    implementation(kotlin("stdlib-jdk7", kotlin_version))
}