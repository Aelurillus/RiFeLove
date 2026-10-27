.pragma library

function youtubeSdrBitrateKbps(height) {
    const h = Number(height) || 1080
    if (h >= 4320) return 200000
    if (h >= 2160) return 68000
    if (h >= 1440) return 24000
    if (h >= 1080) return 12000
    if (h >= 720) return 7500
    if (h >= 480) return 4000
    if (h >= 360) return 1500
    return 1000
}

function localQualityBitrateKbps(height, rate) {
    const h = Number(height) || 1080
    const fps = Number(rate) || 60
    const base = h >= 4320 ? 160000 : h >= 2160 ? 70000 : h >= 1440 ? 40000 :
        h >= 1080 ? 20000 : h >= 720 ? 12000 : 8000
    return Math.min(200000, Math.round(base * Math.min(2, Math.max(1, fps / 60)) / 1000) * 1000)
}

function makePreset(id, height) {
    if (id === "custom") return null
    if (!["youtube60", "bilibili60", "bilibili120", "douyin30", "douyin60"].includes(id)) return null
    const rate = id === "bilibili120" ? "120" : id === "douyin30" ? "30" : "60"
    const bitrateKbps = id === "youtube60" ? youtubeSdrBitrateKbps(height) : localQualityBitrateKbps(height, rate)
    return {
        rate: rate,
        container: "mp4",
        encoder: "auto",
        bitrateKbps: bitrateKbps,
        qualityMode: "bitrate",
        quality: 32,
        gopSize: Number(rate) * 2
    }
}
