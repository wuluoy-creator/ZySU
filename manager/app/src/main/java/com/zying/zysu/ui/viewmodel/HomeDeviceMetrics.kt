package com.zying.zysu.ui.viewmodel

import android.app.ActivityManager
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import android.os.Environment
import android.os.StatFs
import android.os.SystemClock
import java.io.File

/** Missing or denied sources remain null: an unavailable reading is never a zero reading. */
internal data class HomeDeviceMetrics(
    val batteryPercent: Int? = null,
    val batteryTemperatureC: Float? = null,
    val charging: Boolean? = null,
    val cpuTemperatureC: Float? = null,
    val cpuFrequencyKHz: Long? = null,
    val memory: HomeCapacity? = null,
    val storage: HomeCapacity? = null,
    val uptimeMillis: Long = 0L,
    val memoryHistory: List<Float?> = emptyList(),
    val cpuTemperatureHistory: List<Float?> = emptyList(),
)

/** History is bounded to five minutes, and unavailable readings remain gaps in the chart. */
internal fun appendHomeMetricSample(previous: HomeDeviceMetrics?, next: HomeDeviceMetrics): HomeDeviceMetrics = next.copy(
    memoryHistory = (previous?.memoryHistory.orEmpty() + next.memory?.fraction).takeLast(30),
    cpuTemperatureHistory = (previous?.cpuTemperatureHistory.orEmpty() + next.cpuTemperatureC).takeLast(30),
)

internal data class HomeCapacity private constructor(val usedBytes: Long, val totalBytes: Long) {
    val fraction: Float get() = (usedBytes.toDouble() / totalBytes).toFloat()

    companion object {
        fun fromAvailable(totalBytes: Long, availableBytes: Long): HomeCapacity? {
            if (totalBytes <= 0L || availableBytes !in 0L..totalBytes) return null
            return HomeCapacity(totalBytes - availableBytes, totalBytes)
        }
    }
}

internal fun batteryPercentage(level: Int, scale: Int): Int? =
    if (scale <= 0 || level !in 0..scale) null else (level.toLong() * 100L / scale).toInt()

internal fun thermalMilliDegrees(raw: String?): Float? = raw?.trim()?.toFloatOrNull()
    ?.takeIf { it.isFinite() && it in -50_000f..200_000f }
    ?.div(1000f)

internal object HomeDeviceMetricsReader {
    /** Call on an IO dispatcher. No root shell is opened for the device overview. */
    fun read(context: Context): HomeDeviceMetrics {
        val battery = runCatching {
            context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        }.getOrNull()
        val memory = runCatching {
            val info = ActivityManager.MemoryInfo()
            context.getSystemService(ActivityManager::class.java)?.getMemoryInfo(info)
            HomeCapacity.fromAvailable(info.totalMem, info.availMem)
        }.getOrNull()
        val storage = runCatching {
            val stat = StatFs(Environment.getDataDirectory().path)
            HomeCapacity.fromAvailable(stat.totalBytes, stat.availableBytes)
        }.getOrNull()
        return HomeDeviceMetrics(
            batteryPercent = battery?.let {
                batteryPercentage(it.getIntExtra(BatteryManager.EXTRA_LEVEL, -1), it.getIntExtra(BatteryManager.EXTRA_SCALE, -1))
            },
            batteryTemperatureC = battery?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Int.MIN_VALUE)
                ?.takeIf { it in -500..1500 }?.div(10f),
            charging = battery?.getIntExtra(BatteryManager.EXTRA_STATUS, -1)?.let {
                when (it) {
                    BatteryManager.BATTERY_STATUS_CHARGING, BatteryManager.BATTERY_STATUS_FULL -> true
                    BatteryManager.BATTERY_STATUS_DISCHARGING, BatteryManager.BATTERY_STATUS_NOT_CHARGING -> false
                    else -> null
                }
            },
            cpuTemperatureC = readCpuTemperature(),
            cpuFrequencyKHz = readCpuFrequency(),
            memory = memory,
            storage = storage,
            uptimeMillis = SystemClock.elapsedRealtime(),
        )
    }

    private fun readCpuTemperature(): Float? {
        val zones = runCatching { File("/sys/class/thermal").listFiles() }.getOrNull() ?: return null
        return zones.asSequence().filter { it.name.startsWith("thermal_zone") }
            .mapNotNull { zone ->
                val type = readText(File(zone, "type"))?.lowercase() ?: return@mapNotNull null
                val priority = when {
                    type.startsWith("cpu") -> 0
                    type.contains("soc") -> 1
                    else -> return@mapNotNull null
                }
                val value = thermalMilliDegrees(readText(File(zone, "temp"))) ?: return@mapNotNull null
                priority to value
            }.sortedBy { it.first }.firstOrNull()?.second
    }

    private fun readCpuFrequency(): Long? {
        val cores = runCatching { File("/sys/devices/system/cpu").listFiles() }.getOrNull() ?: return null
        return cores.asSequence().filter { it.name.matches(Regex("cpu[0-9]+")) }
            .mapNotNull { core ->
                (readText(File(core, "cpufreq/scaling_cur_freq"))
                    ?: readText(File(core, "cpufreq/cpuinfo_cur_freq")))
                    ?.toLongOrNull()?.takeIf { it > 0L }
            }.maxOrNull()
    }

    private fun readText(file: File): String? = runCatching { file.readText().trim() }.getOrNull()
}
