import AppKit
import BuilderCore

/// Owns the checklist state, runs the pipeline commands and drives the build.
final class BuilderModel: ObservableObject {
    enum Phase: Equatable {
        case checklist
        case building(stepID: String, fraction: Double, log: [String])
        case finished(URL)
        case failed(step: String, message: String, log: [String])
    }

    @Published private(set) var states: [CheckID: CheckState]
    @Published private(set) var phase: Phase = .checklist
    @Published private(set) var gameCopy: URL?
    @Published private(set) var destination: URL
    /// The Gearhead Garage.app already in the destination: the build replaces it (an update).
    @Published private(set) var existingApp: InstalledApp?
    @Published private(set) var cancelDisabled = false
    @Published private(set) var buildFilesRemoved = false
    @Published private(set) var freedBytes: Int64?
    @Published var removeToolsToo = false
    /// A plain message shown as an alert over the checklist.
    @Published var notice: String?
    /// Output of the last failed build tools setup, for Copy details.
    @Published private(set) var setupDetails: String?
    /// Why the game copy was refused (checksums), for Copy details.
    @Published private(set) var gameDetails: String?
    @Published var showingSupported = false

    let paths = BuilderPaths()
    let version = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "0.0.0-dev"
    let sources = BuilderModel.findSources()
    private(set) lazy var manifest: GameManifest? = sources.flatMap {
        try? GameManifest(contentsOf: $0.appendingPathComponent("tools/game_manifest.json"))
    }
    private let defaults = UserDefaults.standard

    private var live: [ObjectIdentifier: PipelineRunner] = [:]
    private var idleHandlers: [() -> Void] = []
    private var workspaceReady = false
    private var installingTools = false
    private var downloadTask: URLSessionDownloadTask?
    private var downloadProgress: NSKeyValueObservation?
    private var cltTimer: Timer?
    /// The game copy was offered by the Builder (the app already in the destination).
    private var gameCopyOffered = false
    private var checkedGame: URL?
    private var gameToken = UUID()

    private var buildRunner: PipelineRunner?
    private var buildLog: [String] = []
    private var buildStep = ""
    private var stepPart = 0.0
    private var buildError: String?
    private var built: URL?
    private var userCancelled = false
    private var flushScheduled = false
    private var autoBuildPending = false

    init() {
        states = Dictionary(uniqueKeysWithValues: CheckID.allCases.map { ($0, CheckState.checking("Checking...")) })
        gameCopy = defaults.string(forKey: "gameCopy").map { URL(fileURLWithPath: $0) }
        destination = defaults.string(forKey: "destination").map { URL(fileURLWithPath: $0, isDirectory: true) } ?? defaultDestination
        offerInstalledApp()
    }

    // MARK: - State for the views

    var phaseName: String {
        switch phase {
        case .checklist: return "checklist"
        case .building: return "building"
        case .finished: return "finished"
        case .failed: return "failed"
        }
    }

    var buildEnabled: Bool { canBuild(states) && sources != nil }
    var buildTitle: String { buildButtonTitle(replacing: existingApp) }
    var hasRunningWork: Bool { !live.isEmpty || downloadTask != nil }

    var workspace: URL { paths.workspace(version: version) }

    var buildFilesBytes: Int64? {
        let size = allocatedSize(of: workspace)
        return size > 0 ? size : nil
    }

    var buildToolsBytes: Int64? {
        let size = allocatedSize(of: paths.python) + allocatedSize(of: paths.venv)
        return size > 0 ? size : nil
    }

    /// The bundled build sources: Contents/Resources/BuildSources, or for a development run
    /// (`swift run` in builder/) the checkout itself, or `-Sources DIR`.
    static func findSources() -> URL? {
        let fm = FileManager.default
        func valid(_ url: URL) -> Bool { fm.fileExists(atPath: url.appendingPathComponent("tools/builder_cli.py").path) }
        if let bundled = Bundle.main.resourceURL?.appendingPathComponent("BuildSources"), valid(bundled) { return bundled }
        if let given = UserDefaults.standard.string(forKey: "Sources").map({ URL(fileURLWithPath: $0) }), valid(given) { return given }
        var folder = Bundle.main.executableURL?.deletingLastPathComponent()
        while let current = folder, current.path != "/" {
            if valid(current) { return current }
            folder = current.deletingLastPathComponent()
        }
        return nil
    }

    /// Identifies the bundled sources (tools/make_builder.py writes .sources-id); a development
    /// run copies the checkout again every time.
    private var workspaceMarker: String {
        let id = sources.flatMap { try? String(contentsOf: $0.appendingPathComponent(".sources-id"), encoding: .utf8) }
        return "\(version)+\(id?.trimmingCharacters(in: .whitespacesAndNewlines) ?? UUID().uuidString)"
    }

    // MARK: - Checklist

    /// With no game copy chosen, a Gearhead Garage.app with the game inside in the destination
    /// is the game copy of an update.
    private func offerInstalledApp() {
        existingApp = InstalledApp.find(in: destination)
        if gameCopy == nil || gameCopyOffered {
            if let app = existingApp, app.hasGame {
                gameCopy = app.url
                gameCopyOffered = true
            } else if gameCopyOffered {
                gameCopy = nil
                gameCopyOffered = false
            }
        }
    }

    func start() {
        autoBuildPending = defaults.bool(forKey: "AutoBuild")
        sweepLeftovers(paths)
        guard let sources = sources else {
            states[.buildTools] = .blocked("The Builder's build sources are missing; download the Builder again")
            refresh()
            return
        }
        refresh()
        let workspace = self.workspace, marker = workspaceMarker, version = self.version, paths = self.paths
        DispatchQueue.global(qos: .userInitiated).async {
            let result = Result { try prepareWorkspace(sources: sources, destination: workspace, version: marker) }
            if case .success = result { removeOldWorkspaces(paths, keeping: version) }
            DispatchQueue.main.async { [self] in
                switch result {
                case .success:
                    workspaceReady = true
                    refreshBuildTools()
                case let .failure(error):
                    states[.buildTools] = .blocked("Could not prepare \(workspace.path): \(error.localizedDescription)")
                }
            }
        }
    }

    func refresh() {
        guard phase == .checklist else { return }
        states[.mac] = macCheck(isAppleSilicon: runningOnAppleSilicon(), version: ProcessInfo.processInfo.operatingSystemVersion)
        DispatchQueue.global(qos: .userInitiated).async {
            let state = CommandLineTools().state()
            DispatchQueue.main.async { [self] in
                if states[.commandLineTools]?.isReady != true || !state.isReady { states[.commandLineTools] = state }
                if state.isReady { cltTimer?.invalidate(); cltTimer = nil }
                maybeAutoBuild()
            }
        }
        offerInstalledApp()
        states[.destination] = destinationCheck(destination)
        refreshDiskSpace()
        refreshBuildTools()
        checkGame()
    }

    private func refreshBuildTools() {
        guard !installingTools, sources != nil else { return }
        let state = workspaceReady ? buildToolsCheck(paths: paths, version: version) : .checking("Preparing...")
        states[.buildTools] = state
        // Unattended runs (-AutoBuild YES) also install the build tools, once.
        if autoBuildPending, state.isBlocked, setupDetails == nil {
            installBuildTools()
            return
        }
        maybeAutoBuild()
    }

    private func refreshDiskSpace() {
        func volume(_ url: URL) -> (id: UInt64, free: Int64, name: String)? {
            var probe = url
            while !FileManager.default.fileExists(atPath: probe.path), probe.path != "/" { probe = probe.deletingLastPathComponent() }
            guard let values = try? probe.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey, .volumeNameKey]),
                  let attributes = try? FileManager.default.attributesOfItem(atPath: probe.path),
                  let device = (attributes[.systemNumber] as? NSNumber)?.uint64Value
            else { return nil }
            return (device, values.volumeAvailableCapacityForImportantUsage ?? 0, values.volumeName ?? probe.path)
        }
        guard let work = volume(paths.support) else {
            states[.diskSpace] = .automatic("Free space could not be checked")
            return
        }
        let target = volume(destination)
        let needs = spaceNeeds(workspace: work.id, destination: target?.id)
        var state = diskSpaceCheck(available: work.free, required: needs[work.id] ?? requiredBuildBytes,
                                   disk: needs.count > 1 ? work.name : nil)
        if let target = target, let need = needs[target.id], target.id != work.id {
            let other = diskSpaceCheck(available: target.free, required: need, disk: target.name)
            if other.isBlocked { state = other }
        }
        states[.diskSpace] = state
    }

    private func checkGame() {
        guard let copy = gameCopy else {
            checkedGame = nil
            gameDetails = nil
            states[.gameCopy] = .blocked("Choose your CD image (.cue, .bin or .iso), GEAR.MSI, the disc or an installed game folder")
            return
        }
        guard copy != checkedGame else { return }
        guard let manifest = manifest else {
            states[.gameCopy] = .blocked("The Builder's game manifest is missing")
            return
        }
        checkedGame = copy
        gameDetails = nil
        let token = UUID()
        gameToken = token
        states[.gameCopy] = .checking("Checking \(copy.lastPathComponent)...")
        DispatchQueue.global(qos: .userInitiated).async {
            let report = checkGameCopy(copy, manifest: manifest)
            DispatchQueue.main.async { [self] in
                guard token == gameToken else { return }
                states[.gameCopy] = report.state
                gameDetails = report.details
                if report.state.isBlocked { checkedGame = nil }
                maybeAutoBuild()
            }
        }
    }

    func setGameCopy(_ url: URL) {
        gameCopy = url
        gameCopyOffered = false
        checkedGame = nil
        defaults.set(url.path, forKey: "gameCopy")
        checkGame()
    }

    func setDestination(_ url: URL) {
        destination = url
        defaults.set(url.path, forKey: "destination")
        refresh()
    }

    /// Dropped files: a single game copy.
    func choose(_ urls: [URL]) {
        guard let url = urls.first else { return }
        if urls.count > 1 { notice = "Drop one game copy: a CD image, GEAR.MSI, the disc or a game folder." }
        setGameCopy(url)
    }

    // MARK: - Installing the tools

    func installCommandLineTools() {
        CommandLineTools().startInstall()
        states[.commandLineTools] = .checking("Follow Apple's installer; this row turns green when it is done")
        cltTimer?.invalidate()
        cltTimer = Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { [weak self] _ in self?.refresh() }
    }

    func installBuildTools() {
        guard !installingTools, workspaceReady else { return }
        installingTools = true
        setupDetails = nil
        if PythonRuntime.isInstalled(paths) {
            runSetup()
            return
        }
        states[.buildTools] = .checking("Downloading Python...")
        PythonRuntime.install(paths, download: { [self] url, done in
            let task = URLSession.shared.downloadTask(with: url) { file, response, error in
                if let error = error { return done(.failure(error)) }
                guard let file = file, (response as? HTTPURLResponse)?.statusCode == 200 else {
                    return done(.failure(DiscError("The Python download failed (HTTP \((response as? HTTPURLResponse)?.statusCode ?? 0))")))
                }
                // The file is deleted when this handler returns; install verifies and unpacks it inside done.
                done(.success(file))
            }
            downloadProgress = task.progress.observe(\.fractionCompleted) { progress, _ in
                let percent = Int(progress.fractionCompleted * 100)
                DispatchQueue.main.async { [self] in
                    guard installingTools else { return }
                    states[.buildTools] = .checking("Downloading Python... \(percent)%")
                }
            }
            downloadTask = task
            task.resume()
        }, completion: { [self] result in
            downloadTask = nil
            downloadProgress = nil
            switch result {
            case .success:
                runSetup()
            case let .failure(error):
                installingTools = false
                setupDetails = error.localizedDescription
                states[.buildTools] = .blocked("Python could not be installed: \(error.localizedDescription)")
                runIdleHandlers()
            }
        })
    }

    private func runSetup() {
        states[.buildTools] = .checking("Installing the translator's packages...")
        var log: [String] = []
        let runner = PipelineRunner.setup(paths: paths, version: version)
        run(runner, onEvent: { event in
            if case let .log(line) = event { log.append(line) }
            if case let .error(text) = event { log.append(text) }
        }, onExit: { [self] code in
            installingTools = false
            if code == 0 {
                refreshBuildTools()
            } else {
                setupDetails = log.joined(separator: "\n")
                states[.buildTools] = .blocked("Setting up the packages failed; check your internet connection and try again")
            }
        })
    }

    // MARK: - Building

    private func maybeAutoBuild() {
        guard autoBuildPending, phase == .checklist, buildEnabled else { return }
        autoBuildPending = false
        build()
    }

    func build() {
        guard buildEnabled, let source = gameCopy, phase == .checklist else { return }
        if !NSRunningApplication.runningApplications(withBundleIdentifier: appBundleID).isEmpty {
            notice = "Quit Gearhead Garage first, then build again."
            return
        }
        buildLog = []
        buildStep = buildSteps[0].id
        stepPart = 0
        buildError = nil
        built = nil
        userCancelled = false
        cancelDisabled = false
        buildFilesRemoved = false
        phase = .building(stepID: buildStep, fraction: 0, log: [])
        let output = workspace.appendingPathComponent("out/\(appName)")
        let runner = PipelineRunner.build(paths: paths, version: version, source: source, output: output)
        buildRunner = runner
        run(runner, onEvent: { [self] event in
            switch event {
            case let .step(id, title):
                _ = title // tools/builder_cli.py also prints it as a log line
                buildStep = id
                stepPart = 0
            case let .progress(part):
                stepPart = part
            case let .done(app):
                built = app
            case let .error(text):
                buildError = text
                buildLog.append(text)
            case let .log(line):
                buildLog.append(line)
            }
            scheduleFlush()
        }, onExit: { [self] code in
            buildRunner = nil
            finishBuild(code)
        })
    }

    /// Publishes the log and progress at most ten times a second.
    private func scheduleFlush() {
        guard !flushScheduled else { return }
        flushScheduled = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { [self] in
            flushScheduled = false
            guard case .building = phase else { return }
            phase = .building(stepID: buildStep, fraction: progressFraction(in: buildStep, part: stepPart), log: buildLog)
        }
    }

    private func finishBuild(_ code: Int32) {
        let log = buildLog
        try? log.joined(separator: "\n").write(to: paths.support.appendingPathComponent("last-build.log"), atomically: true, encoding: .utf8)
        if userCancelled {
            phase = .checklist
            refresh()
            return
        }
        guard code == 0, let app = built else {
            let name = buildStepNames[buildStep] ?? "The build"
            phase = .failed(step: name, message: buildError ?? "The build stopped with exit status \(code).", log: log)
            return
        }
        cancelDisabled = true
        phase = .building(stepID: buildSteps.last!.id, fraction: 1, log: log + ["Moving \(appName) to \(destination.path)"])
        let destination = self.destination
        DispatchQueue.global(qos: .userInitiated).async {
            let result = Result { () -> URL in
                let target = try installApp(app, into: destination)
                try removeQuarantine(fromBuiltApp: target)
                return target
            }
            DispatchQueue.main.async { [self] in
                switch result {
                case let .success(target):
                    existingApp = InstalledApp.find(in: destination)
                    phase = .finished(target)
                case let .failure(error):
                    phase = .failed(step: "Installing the app", message: error.localizedDescription, log: log)
                }
            }
        }
    }

    func cancelBuild() {
        guard let runner = buildRunner, !cancelDisabled else { return }
        userCancelled = true
        cancelDisabled = true
        runner.cancel()
    }

    func tryAgain() {
        phase = .checklist
        checkedGame = nil
        refresh()
    }

    func backToChecklist() { tryAgain() }

    func removeBuildFiles() {
        let fm = FileManager.default
        var targets = [workspace]
        if removeToolsToo { targets += [paths.python, paths.venv] }
        let before = targets.reduce(Int64(0)) { $0 + allocatedSize(of: $1) }
        for target in targets where fm.fileExists(atPath: target.path) {
            if let trash = try? moveToTrash(target) { try? fm.removeItem(at: trash) }
        }
        workspaceReady = false
        freedBytes = before
        buildFilesRemoved = true
    }

    // MARK: - Processes

    private func run(_ runner: PipelineRunner, onEvent: @escaping (PipelineEvent) -> Void, onExit: @escaping (Int32) -> Void) {
        let key = ObjectIdentifier(runner)
        live[key] = runner
        do {
            try runner.start(onEvent: onEvent, onExit: { [self] code in
                live[key] = nil
                onExit(code)
                runIdleHandlers()
            })
        } catch {
            live[key] = nil
            onEvent(.error(error.localizedDescription))
            onExit(127)
        }
    }

    private func runIdleHandlers() {
        guard !hasRunningWork else { return }
        let handlers = idleHandlers
        idleHandlers = []
        handlers.forEach { $0() }
    }

    /// Stops every running command and download; `then` runs once nothing is running.
    func stopAll(then: @escaping () -> Void) {
        downloadTask?.cancel()
        for runner in live.values { runner.cancel() }
        if hasRunningWork { idleHandlers.append(then) } else { then() }
    }

    /// Paths in messages shown without the home folder.
    static func plain(_ text: String) -> String {
        text.replacingOccurrences(of: NSHomeDirectory(), with: "~")
    }
}

func abbreviated(_ url: URL) -> String {
    BuilderModel.plain(url.path)
}
