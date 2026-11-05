import AppKit
import BuilderCore
import SwiftUI
import UniformTypeIdentifiers

struct ChecklistView: View {
    @ObservedObject var model: BuilderModel
    @State private var dropTargeted = false

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 12) {
                if let icon = NSApp.applicationIconImage {
                    Image(nsImage: icon).resizable().frame(width: 44, height: 44)
                }
                VStack(alignment: .leading, spacing: 2) {
                    Text("Gearhead Garage Builder").font(.title2).fontWeight(.semibold)
                    Text("Makes Gearhead Garage.app for Apple Silicon from your own game copy.")
                        .font(.callout)
                        .foregroundColor(.secondary)
                }
            }
            .padding(.bottom, 10)
            ForEach(CheckID.allCases, id: \.self) { id in
                CheckRow(title: id.title, state: model.states[id] ?? .checking("Checking..."), buttons: buttons(for: id))
                if id != CheckID.allCases.last { Divider() }
            }
            Button(action: model.build) {
                Text(model.buildTitle)
                    .fontWeight(.semibold)
                    .frame(maxWidth: .infinity)
            }
            .controlSize(.large)
            .keyboardShortcut(.defaultAction)
            .disabled(!model.buildEnabled)
            .padding(.top, 16)
            Text("No game files come with the Builder. It reads your copy and never changes it.")
                .font(.caption)
                .foregroundColor(.secondary)
                .frame(maxWidth: .infinity)
                .padding(.top, 8)
        }
        .overlay(
            RoundedRectangle(cornerRadius: 8)
                .stroke(Color.accentColor, lineWidth: 3)
                .padding(-10)
                .opacity(dropTargeted ? 1 : 0)
        )
        .onDrop(of: [UTType.fileURL], isTargeted: $dropTargeted, perform: drop)
        .sheet(isPresented: $model.showingSupported) { SupportedCopiesView(model: model) }
    }

    private func buttons(for id: CheckID) -> [RowButton] {
        let state = model.states[id]
        switch (id, state) {
        case (.commandLineTools, .blocked?):
            return [RowButton(label: "Install...", action: model.installCommandLineTools)]
        case (.buildTools, .blocked?):
            let install = RowButton(label: "Install", action: model.installBuildTools)
            guard let details = model.setupDetails else { return [install] }
            return [RowButton(label: "Copy details", action: { copyToPasteboard(details) }), install]
        case (.gameCopy, _):
            var result = [RowButton(label: "Which copy works?", action: { model.showingSupported = true }),
                          RowButton(label: "Choose...", action: chooseCopy)]
            if let details = model.gameDetails {
                result.insert(RowButton(label: "Copy details", action: { copyToPasteboard(details) }), at: 0)
            }
            return result
        case (.destination, _):
            return [RowButton(label: "Change...", action: chooseDestination)]
        default:
            return []
        }
    }

    private func chooseCopy() {
        let panel = NSOpenPanel()
        panel.message = "Choose your Gearhead Garage CD image (.cue, .bin or .iso), GEAR.MSI, the disc, or an installed game folder"
        panel.prompt = "Choose"
        panel.canChooseFiles = true
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        panel.treatsFilePackagesAsDirectories = false
        if let current = model.gameCopy { panel.directoryURL = current.deletingLastPathComponent() }
        if panel.runModal() == .OK, let url = panel.url { model.setGameCopy(url) }
    }

    private func chooseDestination() {
        let panel = NSOpenPanel()
        panel.message = "Choose where to put Gearhead Garage.app"
        panel.prompt = "Choose"
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.canCreateDirectories = true
        panel.allowsMultipleSelection = false
        panel.directoryURL = model.destination
        if panel.runModal() == .OK, let url = panel.url { model.setDestination(url) }
    }

    private func drop(_ providers: [NSItemProvider]) -> Bool {
        guard model.phase == .checklist, !providers.isEmpty else { return false }
        var urls: [URL] = []
        let group = DispatchGroup()
        let lock = NSLock()
        for provider in providers {
            group.enter()
            provider.loadItem(forTypeIdentifier: UTType.fileURL.identifier, options: nil) { item, _ in
                let url = (item as? Data).flatMap { URL(dataRepresentation: $0, relativeTo: nil) } ?? (item as? URL)
                if let url = url {
                    lock.lock()
                    urls.append(url)
                    lock.unlock()
                }
                group.leave()
            }
        }
        group.notify(queue: .main) { model.choose(urls) }
        return true
    }
}

struct RowButton {
    let label: String
    let action: () -> Void
}

func copyToPasteboard(_ text: String) {
    NSPasteboard.general.clearContents()
    NSPasteboard.general.setString(text, forType: .string)
}

struct CheckRow: View {
    let title: String
    let state: CheckState
    let buttons: [RowButton]

    /// Up to two buttons sit beside the message; more go on their own line below it.
    var body: some View {
        HStack(spacing: 12) {
            Circle()
                .fill(state.color)
                .frame(width: 12, height: 12)
            VStack(alignment: .leading, spacing: 2) {
                Text(title).fontWeight(.semibold)
                Text(BuilderModel.plain(state.message))
                    .font(.callout)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                if buttons.count > 2 {
                    HStack { buttonViews }.padding(.top, 6)
                }
            }
            Spacer(minLength: 8)
            if buttons.count <= 2 { buttonViews }
        }
        .padding(.vertical, 9)
    }

    private var buttonViews: some View {
        ForEach(buttons, id: \.label) { button in
            Button(button.label, action: button.action)
        }
    }
}

extension CheckID {
    var title: String {
        switch self {
        case .mac: return "Mac"
        case .commandLineTools: return "Xcode command line tools"
        case .buildTools: return "Build tools (Python)"
        case .gameCopy: return "Your game copy"
        case .destination: return "Destination"
        case .diskSpace: return "Free disk space"
        }
    }
}

extension CheckState {
    var color: Color {
        switch self {
        case .checking: return Color(NSColor.systemGray)
        case .ready: return Color(NSColor.systemGreen)
        case .automatic: return Color(NSColor.systemOrange)
        case .blocked: return Color(NSColor.systemRed)
        }
    }
}
