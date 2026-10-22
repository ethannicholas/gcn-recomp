// The visionOS app: the immersive space the game runs in, and a window for when it cannot.
// The game itself is C++ (visionos_main.mm); this only gets it a CompositorServices layer.
//
// The app opens straight into the space (Info.plist makes it the default scene). If the
// disc image is there and is the right disc, the game starts at once. If not, the launcher
// window opens over the empty space to say why and to copy one in from Files, and the game
// starts once it has one. The space renders into a layer configured for xr_runtime.mm: a
// texture per eye, foveated, sRGB colour and reverse-Z float depth.
import CompositorServices
import SwiftUI
import UIKit
import UniformTypeIdentifiers

struct GameLayerConfiguration: CompositorLayerConfiguration {
    func makeConfiguration(capabilities: LayerRenderer.Capabilities,
                           configuration: inout LayerRenderer.Configuration) {
        configuration.layout = .dedicated
        // Foveated: the only way to more pixels than 1888x1792 a view, since the render
        // quality (vr.txt's `render_quality`, xr_runtime.mm) is refused without it. Only
        // the runtime's compositing pass draws through the rate map; the game's eyes are
        // rendered uniformly into textures of their own and sampled through it.
        configuration.isFoveationEnabled = capabilities.supportsFoveation
        configuration.colorFormat = .bgra8Unorm_srgb
        configuration.depthFormat = .depth32Float
        // The drawables are allocated for the most the device offers; `render_quality`
        // picks how much of that the views use.
        if configuration.isFoveationEnabled {
            configuration.maxRenderQuality = LayerRenderer.RenderQuality(1.0)
        }
    }
}

// The space's layer, and the game that runs into it. Main thread only.
@MainActor
enum Game {
    static var layer: LayerRenderer?
    static var started = false

    // Starts the game if there is a layer for it and a disc it can play: true if it is
    // running.
    static func startIfReady() -> Bool {
        if started { return true }
        guard let layer, String(cString: gcn_vos_check_image()).isEmpty else { return false }
        started = true
        // Retained for good: the game runs until the process ends.
        let ptr = Unmanaged.passRetained(layer).toOpaque()
        let thread = Thread { gcn_vos_run(ptr) }
        thread.name = "render"
        thread.stackSize = 16 << 20
        thread.qualityOfService = .userInteractive
        thread.start()
        return true
    }

    // The scenes the system has given the app, for the log: a space that never shows is a
    // scene the system holds back.
    static func logScenes(_ why: String) {
        var line = "[app] \(why):"
        for session in UIApplication.shared.openSessions {
            let state = session.scene.map { "\($0.activationState.rawValue)" } ?? "no scene"
            line += " \(session.role.rawValue) (\(state))"
        }
        // A file of its own: this can run before the game's log is open.
        let url = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("app.txt")
        let stamped = "\(Date()) \(line)\n"
        if let h = try? FileHandle(forWritingTo: url) {
            h.seekToEndOfFile()
            h.write(stamped.data(using: .utf8)!)
            try? h.close()
        } else {
            try? stamped.write(to: url, atomically: false, encoding: .utf8)
        }
    }

    static func openLauncher() {
        UIApplication.shared.activateSceneSession(for: UISceneSessionActivationRequest(role: .windowApplication),
                                                  errorHandler: nil)
    }
}

// What is wrong with the disc image, and a way to put the right one in.
struct LauncherView: View {
    @Environment(\.dismissWindow) private var dismissWindow
    @State private var problem = String(cString: gcn_vos_check_image())
    @State private var importing = false
    @State private var message = ""

    var body: some View {
        VStack(spacing: 20) {
            Text(String(cString: gcn_vos_app_label()))
                .font(.extraLargeTitle)
            Text(problem.isEmpty ? "Ready." : problem)
                .multilineTextAlignment(.center)
            Button("Choose Disc Image…") { importing = true }
            if !message.isEmpty {
                Text(message).font(.footnote).foregroundStyle(.red)
            }
        }
        .padding(40)
        .frame(minWidth: 520)
        .onAppear {
            // Opened anyway with the game running: not wanted.
            if Game.started { dismissWindow(id: "launcher") }
        }
        .fileImporter(isPresented: $importing, allowedContentTypes: [.data]) { result in
            switch result {
            case .success(let url): copyIn(url)
            case .failure(let error): message = error.localizedDescription
            }
        }
    }

    // Copies the chosen image into Documents, replacing any image already there, and
    // starts the game if it is the right one.
    private func copyIn(_ url: URL) {
        let ext = url.pathExtension.lowercased()
        guard ext == "iso" || ext == "ciso" else {
            message = "That is not an .iso or .ciso."
            return
        }
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        let fm = FileManager.default
        let docs = fm.urls(for: .documentDirectory, in: .userDomainMask)[0]
        do {
            for old in try fm.contentsOfDirectory(at: docs, includingPropertiesForKeys: nil)
            where ["iso", "ciso"].contains(old.pathExtension.lowercased()) {
                try fm.removeItem(at: old)
            }
            try fm.copyItem(at: url, to: docs.appendingPathComponent("game." + ext))
            message = ""
        } catch {
            message = error.localizedDescription
        }
        problem = String(cString: gcn_vos_check_image())
        if Game.startIfReady() { dismissWindow(id: "launcher") }
    }
}

@main
struct GameApp: App {
    @State private var immersion: ImmersionStyle = .full
    @Environment(\.scenePhase) private var phase

    var body: some Scene {
        ImmersiveSpace(id: "game") {
            CompositorLayer(configuration: GameLayerConfiguration()) { layer in
                Game.layer = layer
                Game.logScenes("the space's layer")
                if !Game.startIfReady() { Game.openLauncher() }
            }
        }
        .immersionStyle(selection: $immersion, in: .full)
        // The game's own picture over the viewer's hands and controllers, not under them.
        .upperLimbVisibility(.hidden)
        .onChange(of: phase) { _, now in Game.logScenes("app \(now)") }

        // Never opened by the system, at launch or by restoring a session: only by
        // Game.openLauncher.
        WindowGroup(id: "launcher") {
            LauncherView()
        }
        .windowResizability(.contentSize)
        .defaultLaunchBehavior(.suppressed)
        .restorationBehavior(.disabled)
    }
}
