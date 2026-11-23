vec3 limitBayerHighlights(vec3 linearRgb, vec3 peaks, vec3 gains) {
    vec3 balanced = linearRgb * gains;
    float rawPeak = max(peaks.r, max(peaks.g, peaks.b));
    // Match the CPU renderer. Require bright samples in all three white-balanced
    // channels so clipping in a strongly coloured subject does not remove its colour.
    float commonWhite = min(gains.r, min(gains.g, gains.b));
    vec3 balancedPeaks = peaks * gains;
    float lowestPeak = min(balancedPeaks.r, min(balancedPeaks.g, balancedPeaks.b));
    float highlight = min(rawPeak, lowestPeak / commonWhite);
    float strength = smoothstep(0.90, 0.95, highlight);
    vec3 limited = min(balanced, vec3(commonWhite * rawPeak));
    float neutral = (limited.r + limited.g + limited.b) / 3.0;
    return mix(balanced, vec3(neutral), strength);
}
