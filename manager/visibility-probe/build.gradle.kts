plugins {
    alias(libs.plugins.agp.app)
}

android {
    namespace = "dev.zysu.visibilityprobe"
    defaultConfig {
        applicationId = "dev.zysu.visibilityprobe"
    }
    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }
}
