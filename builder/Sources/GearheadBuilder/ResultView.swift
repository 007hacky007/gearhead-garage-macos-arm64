import AppKit
import BuilderCore
import SwiftUI

struct ResultView: View {
    enum Outcome {
        case success(URL)
        case failure(step: String, message: String, log: [String])
    }

    @ObservedObject var model: BuilderModel
    let outcome: Outcome

    var body: some View {
        switch outcome {
        case let .success(app): success(app)
        case let .failure(step, message, log): failure(step: step, message: message, log: log)
        }
    }

    private func success(_ app: URL) -> some View {
        VStack(spacing: 14) {
            Spacer()
            Image(systemName: "checkmark.circle.fill")
                .font(.system(size: 56))
                .foregroundColor(Color(NSColor.systemGreen))
            Text("Ready").font(.title).fontWeight(.semibold)
            Text("\(appName) is in \(abbreviated(app.deletingLastPathComponent())).")
                .foregroundColor(.secondary)
            Text("The game is inside the app, so you can move it anywhere. Your mechanics and snapshots are kept outside the app, in ~/Library/Application Support/Gearhead Garage.")
                .font(.callout)
                .foregroundColor(.secondary)
                .multilineTextAlignment(.center)
                .fixedSize(horizontal: false, vertical: true)
            HStack {
                Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting([app]) }
                Button("Play") { NSWorkspace.shared.open(app) }
                    .keyboardShortcut(.defaultAction)
            }
            .controlSize(.large)
            Spacer()
            if model.buildFilesRemoved {
                Text("Temporary files deleted\(model.freedBytes.map { ", \(sizeText($0)) freed" } ?? ""). \(appName) and your game copy are unchanged.")
                    .font(.caption)
                    .foregroundColor(.secondary)
                    .multilineTextAlignment(.center)
            } else {
                VStack(spacing: 4) {
                    Button("Delete temporary files\(model.buildFilesBytes.map { " (\(sizeText($0)))" } ?? "")",
                           action: model.removeBuildFiles)
                        .controlSize(.small)
                    Text("Only the Builder's own work files in \(abbreviated(model.workspace)). The app, your game copy and your saves stay.")
                        .font(.caption)
                        .foregroundColor(.secondary)
                        .multilineTextAlignment(.center)
                        .fixedSize(horizontal: false, vertical: true)
                    Toggle("Also delete the build tools\(model.buildToolsBytes.map { " (Python and its packages, \(sizeText($0)))" } ?? "")", isOn: $model.removeToolsToo)
                        .font(.caption)
                }
            }
        }
        .frame(maxWidth: .infinity, minHeight: 430)
    }

    private func failure(step: String, message: String, log: [String]) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 10) {
                Image(systemName: "xmark.octagon.fill")
                    .font(.system(size: 28))
                    .foregroundColor(Color(NSColor.systemRed))
                Text("\(step) failed").font(.title2).fontWeight(.semibold)
            }
            Text(message).fixedSize(horizontal: false, vertical: true)
            Text("Details").font(.headline).padding(.top, 4)
            LogView(lines: log)
            HStack {
                Button("Copy log") { copyToPasteboard(log.joined(separator: "\n")) }
                Spacer()
                Button("Back", action: model.tryAgain)
                    .keyboardShortcut(.defaultAction)
            }
        }
    }
}
