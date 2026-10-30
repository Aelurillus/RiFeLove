.pragma library

function formatDuration(seconds) {
    if (!Number.isFinite(seconds) || seconds < 0)
        return "—"
    const total = Math.ceil(seconds)
    const hours = Math.floor(total / 3600)
    const minutes = Math.floor((total % 3600) / 60)
    const remainder = total % 60
    const two = value => value.toString().padStart(2, "0")
    if (hours > 0)
        return hours + ":" + two(minutes) + ":" + two(remainder)
    if (minutes > 0)
        return minutes + ":" + two(remainder)
    return total + " 秒"
}
