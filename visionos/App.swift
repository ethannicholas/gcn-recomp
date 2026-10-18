// The visionOS app: a launcher window and the immersive space the game runs in. The game
// itself is C++ (visionos_main.mm); this only gets it a CompositorServices layer.
//
// The launcher says whether a disc image is in the app's Documents folder, offers to copy one
// in from Files, and opens the space. The space renders into a layer configured for
// xr_runtime.mm: a texture per eye, no foveation, sRGB colour and reverse-Z float depth.
import CompositorServices
import SwiftUI
import UniformTypeIdentifiers

struct GameLayerConfiguration: CompositorLayerConfiguration {
    func makeConfiguration(capabilities: LayerRenderer.Capabilities,
                           configuration: inout LayerRenderer.Configuration) {
        configuration.layout = .dedicated
        configuration.isFoveationEnabled = false
        configuration.colorFormat = .bgra8Unorm_srgb
        configuration.depthFormat = .depth32Float
    }
}

struct LauncherView: View {
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @State private var image = String(cString: gcn_vos_find_image())
    @State private var importing = false
    @State private var message = ""
    @State private var opening = false

    var body: some View {
        VStack(spacing: 20) {
            Text(String(cString: gcn_vos_app_label()))
                .font(.extraLargeTitle)
            if image.isEmpty {
                Text("Choose your disc image (.iso or .ciso) to copy it into the app.")
                    .multilineTextAlignment(.center)
            } else {
                Text((image as NSString).lastPathComponent)
                    .font(.headline)
                    .foregroundStyle(.secondary)
            }
            HStack(spacing: 16) {
                Button(image.isEmpty ? "Choose Disc Image…" : "Replace Disc Image…") { importing = true }
                Button("Play") {
                    opening = true
                    Task {
                        switch await openImmersiveSpace(id: "game") {
                        case .opened: break
                        default:
                            message = "The immersive space did not open."
                            opening = false
                        }
                    }
                }
                .disabled(image.isEmpty || opening)
            }
            Text("Play with a gamepad, or PlayStation VR2 Sense controllers to aim with your hand.")
                .font(.footnote)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
            if !message.isEmpty {
                Text(message).font(.footnote).foregroundStyle(.red)
            }
        }
        .padding(40)
        .frame(minWidth: 520)
        .fileImporter(isPresented: $importing, allowedContentTypes: [.data]) { result in
            switch result {
            case .success(let url): copyIn(url)
            case .failure(let error): message = error.localizedDescription
            }
        }
    }

    // Copies the chosen image into Documents, replacing any image already there.
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
            image = String(cString: gcn_vos_find_image())
            message = ""
        } catch {
            message = error.localizedDescription
        }
    }
}

@main
struct GameApp: App {
    @State private var immersion: ImmersionStyle = .full

    var body: some Scene {
        WindowGroup {
            LauncherView()
        }
        .windowResizability(.contentSize)

        ImmersiveSpace(id: "game") {
            CompositorLayer(configuration: GameLayerConfiguration()) { layer in
                // Retained for good: the game runs until the process ends.
                let ptr = Unmanaged.passRetained(layer).toOpaque()
                let thread = Thread { gcn_vos_run(ptr) }
                thread.name = "render"
                thread.stackSize = 16 << 20
                thread.qualityOfService = .userInteractive
                thread.start()
            }
        }
        .immersionStyle(selection: $immersion, in: .full)
    }
}
