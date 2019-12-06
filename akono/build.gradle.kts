
plugins {
    id("com.android.library")
    kotlin("android")
    kotlin("android.extensions")
}

android {
    compileSdkVersion(28)
    defaultConfig {
        minSdkVersion(21)
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

        externalNativeBuild {
            cmake.arguments("-DANDROID_STL=c++_shared")
        }
    }
    useLibrary("android.test.runner")
    useLibrary("android.test.base")
    useLibrary("android.test.mock")

    externalNativeBuild {
        cmake {
            setPath(file("src/main/cpp/CMakeLists.txt"))
        }
    }

    sourceSets {
        named("main") {
            jniLibs.srcDirs("../deps/compiled")
        }
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

    androidTestImplementation("androidx.test:core:1.1.0")
    androidTestImplementation("androidx.test:runner:1.1.1")
    androidTestImplementation("androidx.test:rules:1.1.1")

    // Assertions
    androidTestImplementation("androidx.test.ext:junit:1.1.0")
    androidTestImplementation("androidx.test.ext:truth:1.1.0")
    androidTestImplementation("com.google.truth:truth:0.44")

    // Use the Kotlin test library.
    androidTestImplementation("org.jetbrains.kotlin:kotlin-test:$kotlin_version")

    // Use the Kotlin JUnit integration.
    androidTestImplementation("org.jetbrains.kotlin:kotlin-test-junit:$kotlin_version")
    implementation(kotlin("stdlib-jdk7", kotlin_version))
}
