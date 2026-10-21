plugins { id("com.android.application") }
android {
    namespace = "com.zying.zysu.sumhobservertest"
    compileSdk = 37
    buildToolsVersion = "36.1.0"
    ndkVersion = "30.0.16248370"
    defaultConfig {
        applicationId = "com.zying.zysu.sumhobservertest"
        minSdk = 31
        targetSdk = 37
        versionCode = 1
        versionName = "1.0-test"
        testApplicationId = "com.zying.zysu.sumhobservertest.unittest"
        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild { cmake { arguments += "-DANDROID_STL=c++_static"; targets += "sumh_observer_jni" } }
    }
    buildTypes { debug { isDebuggable = true }; release { isMinifyEnabled = false } }
    externalNativeBuild { cmake { path = file("../../native/CMakeLists.txt"); version = "3.22.1" } }
    compileOptions { sourceCompatibility = JavaVersion.VERSION_21; targetCompatibility = JavaVersion.VERSION_21 }
    lint { abortOnError = true; checkReleaseBuilds = false }
    testOptions { unitTests.isReturnDefaultValues = false; unitTests.isIncludeAndroidResources = true }
}
androidComponents { beforeVariants(selector().withBuildType("release")) { it.enable = false } }
dependencies { testImplementation("junit:junit:4.13.2"); testImplementation("org.robolectric:robolectric:4.17") }
