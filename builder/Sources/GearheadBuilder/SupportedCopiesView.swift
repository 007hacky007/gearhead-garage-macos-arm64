import AppKit
import BuilderCore
import SwiftUI

/// "Which copy works?": the one supported release, with the checksums the Builder checks.
struct SupportedCopiesView: View {
    @ObservedObject var model: BuilderModel

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Which copy works?").font(.title2).fontWeight(.semibold)
            Text(model.manifest?.release ?? "Snap-on Gearhead Garage, Build 131")
                .fontWeight(.semibold)
            Text("Sold as the Activision Value release and on the Activision Twin Pack (Snap-On Gear Head Garage & Sprint Car Racing) CD. Any of these works:")
                .fixedSize(horizontal: false, vertical: true)
            VStack(alignment: .leading, spacing: 4) {
                Text("\u{2022} The CD image: a .cue with its .bin, a .bin, .img or .iso")
                Text("\u{2022} The CD itself, or a copy of its files (with GEARHEAD/GEAR.MSI)")
                Text("\u{2022} GEAR.MSI, the installer on the CD")
                Text("\u{2022} The game folder installed on a Windows PC (with ghg.exe)")
                Text("\u{2022} A Gearhead Garage.app this Builder made (to update it)")
            }
            .font(.callout)
            if let manifest = model.manifest {
                VStack(alignment: .leading, spacing: 6) {
                    checksum("GEAR.MSI", manifest.msiSHA256)
                    checksum("ghg.exe (\(manifest.exeSize) bytes)", manifest.exeSHA256)
                }
                Text("Building checks all \(manifest.fileCount) game files (\(sizeText(Int64(manifest.totalBytes)))). An original 1999 retail CD has another installer and is not supported yet.")
                    .font(.callout)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            HStack {
                Spacer()
                Button("OK") { model.showingSupported = false }.keyboardShortcut(.defaultAction)
            }
        }
        .padding(20)
        .frame(width: 520)
    }

    private func checksum(_ name: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 1) {
            Text(name).font(.callout).fontWeight(.semibold)
            Text("SHA-256 \(value)")
                .font(.system(.caption, design: .monospaced))
        }
    }
}
