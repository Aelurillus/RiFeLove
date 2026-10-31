.pragma library

function youtubeSdrBitrateKbps(height, rate) {
    const h = Number(height) || 1080
    const high = Number(rate) >= 48
    if (h >= 4320) return high ? 240000 : 160000
    if (h >= 2160) return high ? 68000 : 45000
    if (h >= 1440) return high ? 24000 : 16000
    if (h >= 1080) return high ? 12000 : 8000
    if (h >= 720) return high ? 7500 : 5000
    if (h >= 480) return high ? 4000 : 2500
    if (h >= 360) return high ? 1500 : 1000
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
    const bitrateKbps = id === "youtube60" ? youtubeSdrBitrateKbps(height, rate) : localQualityBitrateKbps(height, rate)
    return {
        rate: rate,
        container: "mp4",
        encoder: "auto",
        bitrateKbps: bitrateKbps,
        qualityMode: "bitrate",
        quality: 32,
        gopSize: id === "youtube60" ? Math.ceil(Number(rate) / 2) : Number(rate) * 2
    }
}
