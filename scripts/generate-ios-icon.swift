// SPDX-License-Identifier: GPL-3.0-or-later
// Renders the iOS app icon (1024×1024, opaque) from resources/velin.svg: the
// Velin mark centred on a dark indigo background. iOS rounds the corners.
// Usage: swift scripts/generate-ios-icon.swift <velin.svg> <output.png>
import AppKit

let arguments = CommandLine.arguments
guard arguments.count == 3 else {
    FileHandle.standardError.write("Usage: generate-ios-icon.swift <velin.svg> <output.png>\n".data(using: .utf8)!)
    exit(2)
}
let source = try String(contentsOfFile: arguments[1], encoding: .utf8)
// Keep the mark's own definitions and shapes; drop its outer <svg> and title.
guard let open = source.range(of: "<defs>"), let close = source.range(of: "</svg>", options: .backwards) else {
    FileHandle.standardError.write("Unexpected SVG layout\n".data(using: .utf8)!)
    exit(1)
}
let mark = String(source[open.lowerBound..<close.lowerBound])
let size = 1024
// The mark spans x 36–218, y 46–210 in its 256-unit box. Scale it to about
// 55% of the icon and centre it, nudged right toward the triangle's centroid.
let icon = """
<svg xmlns="http://www.w3.org/2000/svg" width="\(size)" height="\(size)" viewBox="0 0 \(size) \(size)">
  <defs>
    <linearGradient id="velinIconBackground" x1="0" y1="0" x2="0.35" y2="1">
      <stop offset="0" stop-color="#1d1c4a"/>
      <stop offset="1" stop-color="#08081a"/>
    </linearGradient>
  </defs>
  <rect width="\(size)" height="\(size)" fill="url(#velinIconBackground)"/>
  <g transform="translate(512 512) scale(3.1) translate(-119 -128)">
\(mark)
  </g>
</svg>
"""
guard let image = NSImage(data: icon.data(using: .utf8)!) else {
    FileHandle.standardError.write("macOS could not render the SVG\n".data(using: .utf8)!)
    exit(1)
}
// Draw into an opaque bitmap: App Store icons must not have an alpha channel.
func fail(_ message: String) -> Never {
    FileHandle.standardError.write((message + "\n").data(using: .utf8)!)
    exit(1)
}
// noneSkipLast: 32-bit pixels whose fourth byte is ignored, so no alpha.
guard let context = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8, bytesPerRow: 0,
                              space: CGColorSpace(name: CGColorSpace.sRGB)!,
                              bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue) else { fail("Cannot create the bitmap") }
context.setFillColor(red: 0.03, green: 0.03, blue: 0.1, alpha: 1)
context.fill(CGRect(x: 0, y: 0, width: size, height: size))
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(cgContext: context, flipped: false)
image.draw(in: NSRect(x: 0, y: 0, width: size, height: size))
NSGraphicsContext.restoreGraphicsState()
guard let rendered = context.makeImage(),
      let png = NSBitmapImageRep(cgImage: rendered).representation(using: .png, properties: [:]) else { fail("Cannot encode the PNG") }
try png.write(to: URL(fileURLWithPath: arguments[2]))
print("Wrote \(arguments[2])")
