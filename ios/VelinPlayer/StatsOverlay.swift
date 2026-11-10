// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors
import SwiftUI
import VelinCore

// Stats for nerds: live stream statistics over the video, refreshed twice a
// second by PlayerModel. The rows come from the shared C++ core, so they match
// the Windows player's panel.
struct StatsOverlay: View {
    let stats: VLNStreamStats?
    let onClose: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                Text("Stats for Nerds").font(.footnote.weight(.semibold))
                Spacer()
                Button(action: onClose) {
                    Image(systemName: "xmark").font(.caption.weight(.bold)).frame(width: 28, height: 28).contentShape(Rectangle())
                }
                .accessibilityLabel("Close Stats")
            }
            .padding(.bottom, 2)
            // Scrolls only when the screen is too short for every row.
            ViewThatFits(in: .vertical) {
                rows
                ScrollView { rows }.scrollBounceBehavior(.basedOnSize)
            }
        }
        .foregroundStyle(.white)
        .padding(12)
        .frame(width: 300)
        .background(.black.opacity(0.62), in: RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(.white.opacity(0.12)))
    }

    private var rows: some View {
        VStack(alignment: .leading, spacing: 6) {
            if let stats {
                ForEach(Array(stats.sections.enumerated()), id: \.offset) { _, section in
                    sectionView(section)
                }
                if stats.isNetwork && stats.speedHistory.count >= 2 {
                    graph("Download speed · peak \(VLNPlayer.formatBitRate(peak(stats.speedHistory) * 8))",
                          stats.speedHistory, color: .blue)
                    graph(String(format: "Buffer ahead · peak %.1f s", peak(stats.bufferHistory)),
                          stats.bufferHistory, color: .green)
                }
            } else {
                Text("Waiting for playback…").foregroundStyle(.secondary)
            }
        }
        .font(.caption2.monospacedDigit())
    }

    private func sectionView(_ section: VLNStatsSection) -> some View {
        VStack(alignment: .leading, spacing: 1) {
            Text(section.title.uppercased()).font(.caption2.weight(.semibold)).foregroundStyle(.cyan)
            ForEach(Array(section.rows.enumerated()), id: \.offset) { _, row in
                HStack(alignment: .firstTextBaseline, spacing: 6) {
                    Text(row.label).foregroundStyle(.white.opacity(0.6)).frame(width: 88, alignment: .leading)
                    Text(row.value).lineLimit(2).frame(maxWidth: .infinity, alignment: .leading)
                }
            }
        }
    }

    private func graph(_ title: String, _ samples: [NSNumber], color: Color) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(title).foregroundStyle(.white.opacity(0.6))
            Sparkline(samples: samples.map(\.doubleValue)).stroke(color, lineWidth: 1.5)
                .frame(height: 26)
                .padding(3)
                .background(.white.opacity(0.06), in: RoundedRectangle(cornerRadius: 4))
        }
    }

    private func peak(_ samples: [NSNumber]) -> Double { samples.map(\.doubleValue).max() ?? 0 }
}

// The newest sample sits at the right edge; a full history (60) spans the width.
private struct Sparkline: Shape {
    let samples: [Double]

    func path(in rect: CGRect) -> Path {
        var path = Path()
        guard samples.count >= 2 else { return path }
        let peak = max(samples.max() ?? 0, 1e-9)
        let step = rect.width / 59
        var x = rect.maxX - step * CGFloat(samples.count - 1)
        for (index, sample) in samples.enumerated() {
            let point = CGPoint(x: x, y: rect.maxY - CGFloat(sample / peak) * rect.height)
            if index == 0 { path.move(to: point) } else { path.addLine(to: point) }
            x += step
        }
        return path
    }
}
