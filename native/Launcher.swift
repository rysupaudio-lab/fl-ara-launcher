// Native app and CLI: no Python, compiler or Apple binaries in the distribution.
import AppKit
import AudioToolbox
import CryptoKit
import Darwin
import CoreServices
import UniformTypeIdentifiers

let launcherVersion = "0.1.0-candidate.13"
let enginePath = "Contents/Libs/FLEngine_x64.dylib"
let executablePath = "Contents/MacOS/OsxFL"
let engineHash = "8d23eebf0fc63fdd1957ba2c8d0f1f1d4e053a9f43173d3c22606c47ace7b76d"
let executableHash = "5be78aa9f9f066f3b28747b48357769b70979e859b42661f2af7163f08e57aab"
let fm = FileManager.default
struct LaunchError: LocalizedError { let message: String; var errorDescription: String? { message } }
func fail(_ message: String) -> LaunchError { LaunchError(message: message) }
func canonical(_ url: URL) -> URL { url.standardizedFileURL.resolvingSymlinksInPath() }
func digest(_ url: URL) throws -> String {
    let stream = try FileHandle(forReadingFrom: url)
    defer { try? stream.close() }
    var hash = SHA256()
    while let data = try stream.read(upToCount: 1024 * 1024), !data.isEmpty { hash.update(data: data) }
    return hash.finalize().map { String(format: "%02x", $0) }.joined()
}
@discardableResult func command(_ executable: String, _ arguments: [String]) throws -> String {
    let task = Process(); task.executableURL = URL(fileURLWithPath: executable); task.arguments = arguments
    let pipe = Pipe(); task.standardOutput = pipe; task.standardError = pipe
    try task.run()
    let data = pipe.fileHandleForReading.readDataToEndOfFile(); task.waitUntilExit()
    let output = String(decoding: data, as: UTF8.self)
    guard task.terminationStatus == 0 else { throw fail("\(URL(fileURLWithPath: executable).lastPathComponent) failed: \(output.trimmingCharacters(in: .whitespacesAndNewlines))") }
    return output
}
func inspect(_ input: URL) throws -> [String: Any] {
    #if !arch(arm64)
    throw fail("This release requires a native Apple Silicon Mac.")
    #endif
    var translated: Int32 = 0; var size = MemoryLayout<Int32>.size
    if sysctlbyname("sysctl.proc_translated", &translated, &size, nil, 0) == 0, translated != 0 {
        throw fail("Run natively on Apple Silicon; Rosetta is unsupported.")
    }
    let app = canonical(input)
    let data = try Data(contentsOf: app.appendingPathComponent("Contents/Info.plist"))
    guard let info = try PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any],
          info["CFBundleIdentifier"] as? String == "com.image-line.flstudio",
          info["CFBundleShortVersionString"] as? String == "26.1.7.5421", info["CFBundleVersion"] as? String == "26.1.7.5421" else {
        throw fail("Unsupported FL Studio installation. This candidate supports only original vendor-signed FL Studio 26.1.7.5421.")
    }
    guard try digest(app.appendingPathComponent(enginePath)) == engineHash,
          try digest(app.appendingPathComponent(executablePath)) == executableHash else { throw fail("FL Studio executable/framework hashes differ from the qualified build. Refusing to launch.") }
    try command("/usr/bin/codesign", ["--verify", "--deep", "--strict", app.path])
    try command("/usr/bin/codesign", ["--verify", "-R=anchor apple generic and identifier \"com.image-line.flstudio\" and certificate leaf[subject.OU] = \"N68WEP5ZZZ\"", app.path])
    return ["source": app.path, "version": "26.1.7.5421", "build": "26.1.7.5421", "engine_sha256": engineHash,
            "executable_sha256": executableHash]
}
let lsregister = "/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"
func unregister(_ app: URL) { _ = try? command(lsregister, ["-u", app.path]) }
func jsonWrite(_ value: [String: Any], _ path: URL) throws {
    try JSONSerialization.data(withJSONObject: value, options: [.prettyPrinted, .sortedKeys]).write(to: path, options: .atomic)
}
func childAlive(_ pid: Int32) -> Bool { kill(pid, 0) == 0 || errno == EPERM }
func rejectSymlinkAncestors(_ url: URL) throws {
    let standardized = url.standardizedFileURL
    var item = standardized
    while item.path != "/" {
        if fm.fileExists(atPath: item.path), canonical(item) != item { throw fail("Private cache ancestors must not contain symlinks.") }
        item.deleteLastPathComponent()
    }
}
func xmlEscape(_ value: String) -> String { value.replacingOccurrences(of: "&", with: "&amp;").replacingOccurrences(of: "<", with: "&lt;").replacingOccurrences(of: ">", with: "&gt;").replacingOccurrences(of: "\"", with: "&quot;") }
final class BatchRenderSettingsReader: NSObject, XMLParserDelegate {
    var keys: [String] = []; var values: [String: String] = [:]
    var name: String?; var text = ""
    let allowed: Set<String> = ["TypeBox", "FormatBox", "QualityBox", "MiscBox", "LMBox", "IPBox", "WFBox", "DitherBox", "NoPolyBox", "PlugHQBox", "BatchBox", "AcidBox", "NoteMarkersBox", "LCBox", "InputFXBox", "MasterFXBox", "PlaylistMarkersBox", "LoopMarkersBox", "ChannelsBox", "SplitMasterBox", "SplitCurrentBox", "OpenRenderLocationBox"]
    func parser(_ parser: XMLParser, didStartElement element: String, namespaceURI: String?, qualifiedName: String?, attributes a: [String: String]) {
        if element == "Key" { keys.append(a["Name"] ?? "") }
        if element == "Value", keys.suffix(4).elementsEqual(["Image-Line", "FL Studio 26", "General", "WAVRenderForm"]),
           a["Type"] == "2", let key = a["Name"], allowed.contains(key) { name = key; text = "" }
    }
    func parser(_ parser: XMLParser, foundCharacters value: String) { if name != nil { text += value } }
    func parser(_ parser: XMLParser, didEndElement element: String, namespaceURI: String?, qualifiedName: String?) {
        if element == "Value", let key = name {
            let value = text.trimmingCharacters(in: .whitespacesAndNewlines)
            if let number = Int(value), (0...1048576).contains(number) { values[key] = value }
            name = nil
        }
        if element == "Key", !keys.isEmpty { keys.removeLast() }
    }
}
func batchRenderSettingsXML() throws -> String {
    let file = fm.homeDirectoryForCurrentUser.appendingPathComponent("Library/Preferences/Image-Line/reg.xml")
    let data = try Data(contentsOf: file)
    guard !data.contains(0), !String(decoding: data, as: UTF8.self).uppercased().contains("<!DOCTYPE") else { throw fail("Unsupported registry XML for the private batch settings copy.") }
    let reader = BatchRenderSettingsReader(), parser = XMLParser(data: data)
    parser.delegate = reader; parser.shouldResolveExternalEntities = false
    guard parser.parse(), !reader.values.isEmpty else { throw fail("Native WAV render settings were not found.") }
    // The audited native WFBox=2 selects float32. Copy numeric render settings
    // only: no registry/license data, paths, account state or whole preferences.
    reader.values["WFBox"] = "2"
    let values = reader.values.keys.sorted().map { "<Value Name=\"\($0)\" Type=\"2\">\(reader.values[$0]!)</Value>" }.joined()
    return "<Key Name=\"General\"><Key Name=\"WAVRenderForm\">\(values)</Key></Key>"
}
func verifyNativeBatchOutput(run: URL, runID: String, pid: Int32) throws -> [String: Any] {
    let resultURL = run.appendingPathComponent("headless-export-result.json")
    guard var result = try JSONSerialization.jsonObject(with: Data(contentsOf: resultURL)) as? [String: Any],
          result["run_id"] as? String == runID, result["pid"] as? Int32 == pid,
          result["route"] as? String == "normal_native_cli", result["requested"] as? Bool == true,
          let outputPath = result["output"] as? String else {
        throw fail("The owned native batch did not leave its matching observation receipt.")
    }
    // The stock CLI may exit the process without returning CreateFruityInstance.
    // Session already verified waitpid exit 0. Require our real loader-return
    // preparation event as well; do not invent a native function-return proof.
    let log = try String(contentsOf: run.appendingPathComponent("native-app.jsonl"), encoding: .utf8)
    let primed = log.split(separator: "\n").contains { line in
        guard let data = String(line).data(using: .utf8),
              let event = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return false }
        return event["event"] as? String == "native_batch_prime_ready" && event["run_id"] as? String == runID && event["pid"] as? Int32 == pid
    }
    guard primed else { throw fail("ARA preparation at the genuine native loader-return boundary was not confirmed.") }
    let output = URL(fileURLWithPath: outputPath)
    guard output.deletingLastPathComponent() == run.appendingPathComponent("export"), output.pathExtension == "wav" else { throw fail("Unexpected native batch output path.") }
    try rejectSymlinkAncestors(output)
    let attributes = try fm.attributesOfItem(atPath: output.path)
    guard attributes[.type] as? FileAttributeType == .typeRegular,
          (attributes[.ownerAccountID] as? NSNumber)?.uint32Value == getuid() else { throw fail("The native batch output is not an owned regular file.") }
    var audio: AudioFileID?
    guard AudioFileOpenURL(output as CFURL, .readPermission, kAudioFileWAVEType, &audio) == noErr, let audio else { throw fail("Native batch did not produce readable WAVE audio.") }
    defer { AudioFileClose(audio) }
    var format = AudioStreamBasicDescription(); var size = UInt32(MemoryLayout.size(ofValue: format))
    guard AudioFileGetProperty(audio, kAudioFilePropertyDataFormat, &size, &format) == noErr,
          format.mFormatID == kAudioFormatLinearPCM, format.mSampleRate == 48000, format.mChannelsPerFrame == 2,
          format.mBitsPerChannel == 32, (format.mFormatFlags & kAudioFormatFlagIsFloat) != 0,
          format.mFramesPerPacket == 1, format.mBytesPerFrame == 8 else { throw fail("Native batch output must be float32 stereo at 48 kHz; output is preserved.") }
    var frames: UInt64 = 0; size = UInt32(MemoryLayout.size(ofValue: frames))
    guard AudioFileGetProperty(audio, kAudioFilePropertyAudioDataPacketCount, &size, &frames) == noErr, frames > 0 else { throw fail("Native batch output has no frames.") }
    var bytes: UInt64 = 0; size = UInt32(MemoryLayout.size(ofValue: bytes))
    guard AudioFileGetProperty(audio, kAudioFilePropertyAudioDataByteCount, &size, &bytes) == noErr,
          frames <= UInt64.max / 8, bytes == frames * 8 else { throw fail("Native batch WAVE extent is incomplete.") }
    result.merge(["state": "native_cli_output_verified", "completed": true, "output_verified": true,
                  "completion_basis": "owned_process_exit_0_and_verified_output", "batch_primed_before_export": true,
                  "sample_rate": 48000, "channels": 2, "bits_per_sample": 32, "sample_format": "float",
                  "frames": frames, "output_sha256": try digest(output)]) { _, new in new }
    try jsonWrite(result, resultURL); return result
}
func preparePrivateHome(_ home: URL, tmp: URL, userData: URL, testDevice: String? = nil, batchExport: Bool = false) throws {
    for folder in [home, tmp, userData, home.appendingPathComponent("Library/Preferences/Image-Line"), home.appendingPathComponent("Library/Application Support"), home.appendingPathComponent("Documents"), home.appendingPathComponent("Desktop")] {
        try rejectSymlinkAncestors(folder)
        try fm.createDirectory(at: folder, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
    }
    // Deliberately synthesize only path settings. Never read or duplicate license keys,
    // the original Image-Line registry, whole home folders or write-through symlinks.
    let device = testDevice == nil && !batchExport ? "" : "<Value Name=\"Device name\" Type=\"2\">4Pro Tools Audio Bridge 2-A</Value>"
    let input = batchExport ? "<Key Name=\"Audio Input\">\(device)</Key>" : ""
    let render = batchExport ? try batchRenderSettingsXML() : ""
    let inputDisabled = batchExport ? "<Key Name=\"DSTITF\"><Value Name=\"InputDisabled\" Type=\"2\">1</Value></Key>" : ""
    let audio = "<Key Name=\"FL Studio 26\"><Key Name=\"Devices\"><Key Name=\"Audio output\"><Value Name=\"Sample rate\" Type=\"2\">48000</Value>\(device)</Key>\(input)</Key>\(render)\(inputDisabled)</Key>"
    let xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><XMLReg><Key Name=\"Key1\"><Key Name=\"HKEY_CURRENT_USER\"><Key Name=\"Software\"><Key Name=\"Image-Line\"><Key Name=\"Shared\"><Key Name=\"Paths\"><Value Name=\"Shared data\" Type=\"2\">\(xmlEscape(userData.path + "/"))</Value></Key></Key>\(audio)</Key></Key></Key></Key></XMLReg>"
    let registry = home.appendingPathComponent("Library/Preferences/Image-Line/reg.xml")
    try Data(xml.utf8).write(to: registry, options: .atomic)
    try fm.setAttributes([.posixPermissions: 0o600], ofItemAtPath: registry.path)
}
final class SharedPathReader: NSObject, XMLParserDelegate {
    var stack: [(String, String?)] = []; var captured: [String] = []; var text = ""; var active = false
    func parser(_ parser: XMLParser, didStartElement element: String, namespaceURI: String?, qualifiedName: String?, attributes attributesDict: [String: String]) {
        stack.append((element, attributesDict["Name"]))
        if element == "Value", attributesDict["Name"] == "Shared data", attributesDict["Type"] == "2",
           stack.dropLast().filter({ $0.0 == "Key" }).compactMap({ $0.1 }).suffix(5).elementsEqual(["HKEY_CURRENT_USER", "Software", "Image-Line", "Shared", "Paths"]) { active = true; text = "" }
    }
    func parser(_ parser: XMLParser, foundCharacters string: String) { if active { text += string } }
    func parser(_ parser: XMLParser, didEndElement element: String, namespaceURI: String?, qualifiedName: String?) {
        if active && element == "Value" { captured.append(text); active = false }; if !stack.isEmpty { stack.removeLast() }
    }
}
func normalUserDataPath() throws -> URL {
    let realHome = fm.homeDirectoryForCurrentUser
    let registry = realHome.appendingPathComponent("Library/Preferences/Image-Line/reg.xml")
    guard fm.fileExists(atPath: registry.path) else { return realHome.appendingPathComponent("Documents/Image-Line") }
    let data = try Data(contentsOf: registry)
    guard !data.contains(0), !String(decoding: data, as: UTF8.self).uppercased().contains("<!DOCTYPE") else { throw fail("Unexpected Image-Line registry XML; refusing a fallback profile.") }
    let reader = SharedPathReader(), parser = XMLParser(data: data); parser.delegate = reader; parser.shouldResolveExternalEntities = false
    guard parser.parse(), reader.captured.count == 1, reader.captured[0].hasPrefix("/") else { throw fail("Cannot determine exact existing FL user-data location without guessing.") }
    let result = canonical(URL(fileURLWithPath: reader.captured[0]))
    guard !["/", realHome.path, realHome.appendingPathComponent("Documents").path, realHome.appendingPathComponent("Library").path].contains(result.path) else { throw fail("Configured FL user-data path is too broad for safe isolation.") }
    return result
}
func physicalPath(_ url: URL) -> String {
    if let pointer = realpath(url.path, nil) { defer { free(pointer) }; return String(cString: pointer) }
    guard url.path != "/" else { return "/" }
    let parent = physicalPath(url.deletingLastPathComponent())
    return (parent == "/" ? "" : parent) + "/" + url.lastPathComponent
}
func sandboxLiteral(_ path: String) throws -> String {
    guard !path.contains("\n"), !path.contains("\r"), !path.contains("\0") else { throw fail("Unsupported control character in protected path.") }
    return "\"" + path.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"") + "\""
}
func prepareSandbox(_ run: URL, source: URL, normalData: URL) throws -> (URL, URL) {
    guard fm.isExecutableFile(atPath: "/usr/bin/sandbox-exec") else { throw fail("Required child-only write sandbox is unavailable.") }
    let home = fm.homeDirectoryForCurrentUser
    var protected = [source, home.appendingPathComponent("Library/Preferences/Image-Line"), normalData,
                     URL(fileURLWithPath: "/Library/Audio/Plug-Ins"), home.appendingPathComponent("Library/Audio/Plug-Ins")]
    let preferences = home.appendingPathComponent("Library/Preferences")
    if let entries = try? fm.contentsOfDirectory(at: preferences, includingPropertiesForKeys: nil) {
        protected += entries.filter { $0.lastPathComponent.hasPrefix("com.image-line.") }
    }
    // Also protect the backing bundles of directly installed plugin symlinks.
    // Enumerate only the named plugin directories; never recurse through links.
    for base in [URL(fileURLWithPath: "/Library/Audio/Plug-Ins"), home.appendingPathComponent("Library/Audio/Plug-Ins")] {
        for type in ["Components", "VST3", "VST"] {
            if let bundles = try? fm.contentsOfDirectory(at: base.appendingPathComponent(type), includingPropertiesForKeys: nil) {
                protected += bundles.filter { ["component", "vst3", "vst"].contains($0.pathExtension.lowercased()) }
            }
        }
    }
    let paths = try protected.map { "(subpath " + (try sandboxLiteral(physicalPath($0))) + ")" }.joined(separator: " ")
    let profile = run.appendingPathComponent("write-sandbox.sb"), trampoline = run.appendingPathComponent("exec-private-fl.sh")
    try Data(("(version 1) (allow default) (deny file-write* (require-any " + paths + "))\n").utf8).write(to: profile, options: .atomic)
    try Data("#!/bin/sh\nset -eu\nif [ \"${RYSUP_FLARA_BATCH_EXPORT:-0}\" = 1 ]; then\nexport DYLD_INSERT_LIBRARIES=\"$RYSUP_FLARA_HELPER_PATH:$RYSUP_FLARA_RATE_HOLD_PATH\"\nelse\nexport DYLD_INSERT_LIBRARIES=\"$RYSUP_FLARA_HELPER_PATH\"\nfi\nexec \"$RYSUP_FLARA_EXPECTED_EXE\" \"$@\"\n".utf8).write(to: trampoline, options: .atomic)
    for item in [profile, trampoline] { try fm.setAttributes([.posixPermissions: 0o600], ofItemAtPath: item.path) }
    return (profile, trampoline)
}
@discardableResult func validateHelper(_ helper: URL) throws -> [String: Any] {
    let manifest = helper.deletingLastPathComponent().appendingPathComponent("helper.json")
    guard let row = try JSONSerialization.jsonObject(with: Data(contentsOf: manifest)) as? [String: Any],
          row["schema"] as? String == "rysup.fl-ara-helper/1", row["runtime_ready"] as? Bool == true,
          row["binary"] as? String == helper.lastPathComponent, row["sha256"] as? String == (try digest(helper)) else {
        throw fail("The FL native helper is absent, unsealed, or still a placeholder. Runtime launch is disabled.")
    }
    try command("/usr/bin/codesign", ["--verify", "--strict", helper.path])
    return row
}
func handlerSnapshot() -> String? {
    guard let type = UTType(filenameExtension: "flp") else { return nil }
    return LSCopyDefaultRoleHandlerForContentType(type.identifier as CFString, LSRolesMask.all)?.takeRetainedValue() as String?
}
struct HandlerDecision {
    let original: String?; let current: String?
    var externalChange: Bool { original != current }
    // Private FL copies retain the vendor bundle ID. A different selected
    // bundle ID is never authority for this launcher to overwrite user choice.
    var defaultWriteAuthorized: Bool { false }
    var receipt: [String: Any] {
        ["original_flp_handler": original as Any? ?? NSNull(), "current_flp_handler": current as Any? ?? NSNull(),
         "external_handler_change": externalChange, "default_handler_write_performed": false,
         "flp_handler_observation_available": original != nil && current != nil,
         "flp_handler_preserved": original != nil && current != nil ? !externalChange as Any : NSNull()]
    }
}
func handlerDecision(original: String?, current: String?) -> HandlerDecision { HandlerDecision(original: original, current: current) }
// Enumerate exact executable paths, including unbridged duplicates created by
// LaunchServices or another UI tool. Never signal or adopt any of these PIDs.
func exactPrivateProcessPIDs(_ observed: [(pid: Int32, path: String)], expectedPhysicalPath: String) -> [Int32] {
    observed.filter { $0.path == expectedPhysicalPath }.map { $0.pid }.sorted()
}
func liveExactProcesses(_ executable: URL) throws -> [Int32] {
    guard let handle = dlopen("/usr/lib/libproc.dylib", RTLD_NOW | RTLD_LOCAL) else { throw fail("Cannot verify all processes before cleanup; preserve this app copy.") }
    defer { dlclose(handle) }
    typealias List = @convention(c) (UnsafeMutableRawPointer?, Int32) -> Int32
    typealias Path = @convention(c) (Int32, UnsafeMutableRawPointer?, UInt32) -> Int32
    guard let ls = dlsym(handle, "proc_listallpids"), let ps = dlsym(handle, "proc_pidpath") else { throw fail("Process inventory is unavailable; preserve this app copy.") }
    let list = unsafeBitCast(ls, to: List.self), path = unsafeBitCast(ps, to: Path.self)
    let needed = list(nil, 0)
    guard needed > 0 else { throw fail("Process inventory failed; preserve this app copy.") }
    var pids = [Int32](repeating: 0, count: Int(needed) + 1024)
    let count = pids.withUnsafeMutableBytes { list($0.baseAddress, Int32($0.count)) }
    guard count >= 0, count < pids.count else { throw fail("Process inventory changed too quickly; preserve this app copy.") }
    let expected = physicalPath(executable)
    var observed: [(pid: Int32, path: String)] = []
    for pid in pids.prefix(Int(count)) where pid > 0 {
        var bytes = [CChar](repeating: 0, count: 4096)
        let length = bytes.withUnsafeMutableBytes { path(pid, $0.baseAddress, UInt32($0.count)) }
        if length > 0 { observed.append((pid, String(cString: bytes))) }
    }
    return exactPrivateProcessPIDs(observed, expectedPhysicalPath: expected)
}
func sourceLogicalBytes(_ source: URL) throws -> UInt64 {
    var traversalError: Error?
    guard let entries = fm.enumerator(at: source, includingPropertiesForKeys: [.isRegularFileKey, .isSymbolicLinkKey, .fileSizeKey], options: [], errorHandler: { _, error in traversalError = error; return false }) else { throw fail("Cannot size the selected FL application safely.") }
    var bytes: UInt64 = 0
    for case let file as URL in entries {
        let values = try file.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey, .fileSizeKey])
        if values.isSymbolicLink == true { entries.skipDescendants(); continue }
        if values.isRegularFile == true { bytes += UInt64(values.fileSize ?? 0) }
    }
    if let traversalError { throw traversalError }
    return bytes
}
func freeBytes(_ directory: URL) throws -> UInt64 { (try fm.attributesOfFileSystem(forPath: directory.path)[.systemFreeSize] as? NSNumber)?.uint64Value ?? 0 }
// This exact supported vendor build reads template sidecars through an FPC Text
// filename buffer of 255 UTF-16 units. Its longest bundled template .nfo suffix,
// including the leading slash after the app path, is 103 units.
let longestTemplateSidecarSuffixUnits = 103
func validatePrivateAppPath(_ physicalAppPath: String) throws {
    let total = physicalAppPath.utf16.count + longestTemplateSidecarSuffixUnits
    guard total <= 255 else {
        throw fail("The private FL app location is too long for this FL build's template reader (\(total)/255 UTF-16 units). Choose a shorter real --cache-root location; symlinks and automatic fallback locations are not used.")
    }
}
func copyPrivateApp(_ source: URL, to destination: URL, cache: URL) throws -> String {
    // Refuse before examining copy inputs or creating any vendor app payload.
    try validatePrivateAppPath(physicalPath(destination))
    let reserve: UInt64 = 5 * 1024 * 1024 * 1024
    let fromFS = try fm.attributesOfFileSystem(forPath: source.path)[.systemNumber] as? NSNumber
    let toFS = try fm.attributesOfFileSystem(forPath: cache.path)[.systemNumber] as? NSNumber
    guard try freeBytes(cache) >= reserve else { throw fail("At least 5 GiB free is required before preparing the private app copy.") }
    if fromFS != nil, fromFS == toFS {
        do { try command("/bin/cp", ["-cR", source.path, destination.path]); return "apfs_clone" }
        catch {
            guard try liveExactProcesses(destination.appendingPathComponent(executablePath)).isEmpty else { throw fail("A live private process prevents partial-copy cleanup.") }
            if fm.fileExists(atPath: destination.path) { try fm.removeItem(at: destination) }
        }
    }
    // A full fallback is allowed only on the owner's explicitly selected real
    // Razer mount. Never silently fill the internal disk with a full app copy.
    guard cache.path.hasPrefix("/Volumes/Razer SSD/"), fm.fileExists(atPath: "/Volumes/Razer SSD"), fromFS != toFS else { throw fail("APFS cloning failed or crosses volumes. Select --cache-root on the mounted Razer SSD; no internal full-copy fallback is allowed.") }
    let logical = try sourceLogicalBytes(source)
    guard try freeBytes(cache) >= logical + reserve else { throw fail("The external destination needs the entire app size plus a 5 GiB reserve.") }
    try command("/usr/bin/ditto", ["--rsrc", "--extattr", "--acl", source.path, destination.path])
    guard try freeBytes(cache) >= reserve else { throw fail("Private app copy breached the free-space reserve.") }
    return "explicit_razer_full_copy"
}
enum SessionCleanupDecision: String {
    case removeExitedCopy, preserveLiveChild, preserveExactPrivateProcesses, preserveUnknownProcessInventory
}
func sessionCleanupDecision(childRunning: Bool, exactPrivatePIDs: [Int32]?) -> SessionCleanupDecision {
    if childRunning { return .preserveLiveChild }
    guard let exactPrivatePIDs else { return .preserveUnknownProcessInventory }
    return exactPrivatePIDs.isEmpty ? .removeExitedCopy : .preserveExactPrivateProcesses
}
func startupExitMessage(_ code: Int32?, diagnostics: URL) -> String {
    if code == 78 { return "The private FL copy refused startup (exit 78). See diagnostics: \(diagnostics.path)" }
    if let code { return "The private FL copy exited before startup was confirmed (exit \(code)). See diagnostics: \(diagnostics.path)" }
    return "The private FL copy exited before startup was confirmed; its exit status was not observed. See diagnostics: \(diagnostics.path)"
}
func ownedStartupRefusalEvent(log: URL, runID: String, pid: Int32) -> String? {
    guard let handle = try? FileHandle(forReadingFrom: log) else { return nil }
    defer { try? handle.close() }
    let size = (try? handle.seekToEnd()) ?? 0
    try? handle.seek(toOffset: size > 65536 ? size - 65536 : 0)
    let text = String(decoding: (try? handle.readToEnd()) ?? Data(), as: UTF8.self)
    let admitted = Set(["owned_startup_save_guard_refused", "owned_startup_engine_identity_refused", "owned_startup_create_contract_refused"])
    for line in text.split(separator: "\n").reversed() {
        guard let row = try? JSONSerialization.jsonObject(with: Data(line.utf8)) as? [String: Any],
              row["run_id"] as? String == runID, row["pid"] as? Int32 == pid,
              let event = row["event"] as? String, admitted.contains(event) else { continue }
        return event
    }
    return nil
}
final class OwnedChild {
    private(set) var processIdentifier: Int32 = 0
    private(set) var terminationStatus: Int32 = 0
    private(set) var exitStatusObserved = false
    private var finished = false
    init(executable: URL, arguments: [String], environment: [String: String], directory: URL, console: FileHandle) throws {
        var actions: posix_spawn_file_actions_t?; var attributes: posix_spawnattr_t?
        posix_spawn_file_actions_init(&actions); posix_spawnattr_init(&attributes)
        defer { posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attributes) }
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0)
        posix_spawn_file_actions_adddup2(&actions, console.fileDescriptor, STDOUT_FILENO)
        posix_spawn_file_actions_adddup2(&actions, console.fileDescriptor, STDERR_FILENO)
        posix_spawn_file_actions_addchdir_np(&actions, directory.path)
        // A terminal interrupt to the launcher must never terminate the user's FL Studio session.
        posix_spawnattr_setpgroup(&attributes, 0)
        posix_spawnattr_setflags(&attributes, Int16(POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_CLOEXEC_DEFAULT))
        var argv = ([executable.path] + arguments).map { strdup($0) } + [nil]
        var envp = environment.sorted { $0.key < $1.key }.map { strdup("\($0.key)=\($0.value)") } + [nil]
        defer { argv.forEach { free($0) }; envp.forEach { free($0) } }
        let result = argv.withUnsafeMutableBufferPointer { av in envp.withUnsafeMutableBufferPointer { ev in
            posix_spawn(&processIdentifier, executable.path, &actions, &attributes, av.baseAddress!, ev.baseAddress!)
        } }
        guard result == 0 else { throw fail("Could not start the private FL Studio copy: \(String(cString: strerror(result)))") }
    }
    var isRunning: Bool {
        guard !finished else { return false }
        var status: Int32 = 0
        let result = waitpid(processIdentifier, &status, WNOHANG)
        if result == 0 || (result < 0 && errno == EINTR) { return true }
        if result < 0 { return childAlive(processIdentifier) }
        finished = true; exitStatusObserved = true
        terminationStatus = status & 0x7f == 0 ? (status >> 8) & 0xff : 128 + (status & 0x7f)
        return false
    }
    func waitUntilExit() { while isRunning { Thread.sleep(forTimeInterval: 0.2) } }
}
func cleanupFinishedSession(_ cache: URL) throws -> String {
    try rejectSymlinkAncestors(cache)
    let lock = cache.appendingPathComponent("active.lock")
    guard fm.fileExists(atPath: lock.path) else { return "There is no previous session to clean." }
    guard canonical(lock) == lock else { throw fail("The session lock is a symlink. Preserve it for manual review.") }
    let owner = try JSONSerialization.jsonObject(with: Data(contentsOf: lock.appendingPathComponent("owner.json"))) as? [String: Any]
    guard let path = owner?["session"] as? String else { throw fail("The previous session receipt is missing. Preserve the cache for manual review.") }
    let manifest = canonical(URL(fileURLWithPath: path)); let runs = canonical(cache.appendingPathComponent("runs"))
    guard manifest.lastPathComponent == "session.json", manifest.deletingLastPathComponent().deletingLastPathComponent() == runs else { throw fail("The previous session receipt is outside this launcher's runs directory.") }
    var receipt = try JSONSerialization.jsonObject(with: Data(contentsOf: manifest)) as? [String: Any] ?? [:]
    let app = manifest.deletingLastPathComponent().appendingPathComponent("FL Studio 2026.app")
    guard receipt["owned_app"] as? String == app.path,
          receipt["run_directory"] as? String == manifest.deletingLastPathComponent().path else { throw fail("The previous receipt does not identify this exact private app. Preserve it for manual review.") }
    // Any live PID is a refusal, including PID reuse. Never adopt or signal it.
    for key in ["launcher_pid", "pid"] {
        let value = key == "launcher_pid" ? owner?[key] : receipt[key]
        if let pid = value as? Int32, pid != getpid(), childAlive(pid) { throw fail("A previous launcher or FL Studio PID is still alive. Quit that session normally before cleanup.") }
    }
    let remaining = try liveExactProcesses(app.appendingPathComponent(executablePath))
    guard remaining.isEmpty else { throw fail("Private FL executables are still running (PIDs \(remaining)); preserve the app and lock until every copy exits normally.") }
    if fm.fileExists(atPath: app.path) {
        guard canonical(app) == app else { throw fail("The previous app path is a symlink. Preserve it for manual review.") }
        unregister(app); try fm.removeItem(at: app)
    }
    receipt.merge(handlerDecision(original: receipt["original_flp_handler"] as? String, current: handlerSnapshot()).receipt) { _, new in new }
    receipt["cleanup_pending"] = nil; receipt["status"] = "cleaned_after_exit"; try jsonWrite(receipt, manifest)
    try fm.removeItem(at: lock.appendingPathComponent("owner.json")); try fm.removeItem(at: lock)
    return "The exited session's private application copy was removed. Its diagnostics were kept."
}
final class Session {
    let cache: URL; let helper: URL
    var process: OwnedChild?
    var changed: (String) -> Void = { _ in }
    init(cache: URL, helper: URL) { self.cache = cache.standardizedFileURL; self.helper = canonical(helper) }
    func launch(source: URL, project: URL?, testDevice: String? = nil, headlessExport: Bool = false) throws {
        guard !headlessExport || (project != nil && testDevice == nil) else { throw fail("--headless-export requires a backed-up project and normal physical-device mode.") }
        if let testDevice, testDevice != "Pro Tools Audio Bridge 2-A_UID" { throw fail("Test mode permits only Pro Tools Audio Bridge 2-A_UID.") }
        let admission = try validateHelper(helper)
        if admission["runtime_admission_scope"] as? String == "bare-startup-test-only" {
            guard testDevice == "Pro Tools Audio Bridge 2-A_UID", project == nil else { throw fail("This owner-local test candidate admits only a blank startup with the exact virtual test device; project loading and normal audio are disabled.") }
        }
        let source = canonical(source)
        var receipt = try inspect(source)
        guard fm.fileExists(atPath: helper.path), !helper.path.contains(":") else { throw fail("The bundled native bridge is missing or its path contains a colon.") }
        let helperHash = try digest(helper)
        let rateHold = helper.deletingLastPathComponent().appendingPathComponent("RysUpRateHold.dylib")
        if headlessExport {
            guard !rateHold.path.contains(":"), canonical(rateHold) == rateHold,
                  admission["rate_hold_sha256"] as? String == (try digest(rateHold)) else { throw fail("The bundled rate-hold companion is missing or unsealed.") }
            try command("/usr/bin/codesign", ["--verify", "--strict", rateHold.path])
        }
        let normalData = try normalUserDataPath()
        if let project { guard project.pathExtension == "flp", fm.fileExists(atPath: project.path) else { throw fail("Choose an existing, backed-up .flp project.") } }
        try rejectSymlinkAncestors(cache)
        try fm.createDirectory(at: cache, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        let lock = cache.appendingPathComponent("active.lock")
        guard Darwin.mkdir(lock.path, 0o700) == 0 else { throw fail("A launcher session is active, or needs cleanup. Quit its FL Studio copy normally, then choose Clean finished session.") }
        let runID = UUID().uuidString.lowercased()
        let run = cache.appendingPathComponent("runs/\(runID)")
        do { try fm.createDirectory(at: run, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700]) }
        catch { try? fm.removeItem(at: lock); throw error }
        let app = run.appendingPathComponent("FL Studio 2026.app"), manifest = run.appendingPathComponent("session.json")
        let log = run.appendingPathComponent("native-app.jsonl")
        let originalHandler = handlerSnapshot()
        receipt.merge(["launcher_version": launcherVersion, "launcher_implementation": "native-swift", "launcher_pid": getpid(), "run_id": runID, "run_directory": run.path,
                       "owned_app": app.path, "original_flp_handler": originalHandler as Any? ?? NSNull(), "normal_user_data_protected": normalData.path, "mode": testDevice == nil ? "normal" : "test", "project": project?.path as Any? ?? NSNull(), "helper_sha256": helperHash, "status": "preparing"]) { _, new in new }
        var operationError: Error?
        var nativeStartupConfirmed = false
        do {
            if headlessExport, let project {
                receipt["headless_export_requested"] = true
                let outputDirectory = run.appendingPathComponent("export")
                try fm.createDirectory(at: outputDirectory, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
                let output = outputDirectory.appendingPathComponent(project.deletingPathExtension().lastPathComponent + ".wav")
                guard !fm.fileExists(atPath: output.path) else { throw fail("Owned export output already exists.") }
                try jsonWrite(["schema": "rysup.fl-ara-owned-export/1", "run_id": runID,
                               "project": project.path, "project_sha256": try digest(project),
                               "output": output.path], run.appendingPathComponent("headless-export-request.json"))
                try fm.setAttributes([.posixPermissions: 0o600], ofItemAtPath: run.appendingPathComponent("headless-export-request.json").path)
            }
            try jsonWrite(receipt, manifest)
            try jsonWrite(["launcher_pid": getpid(), "session": manifest.path], lock.appendingPathComponent("owner.json"))
            changed("Verifying and preparing a private FL Studio copy…")
            receipt["copy_mode"] = try copyPrivateApp(source, to: app, cache: cache)
            let copy = try inspect(app)
            for key in ["engine_sha256", "executable_sha256"] {
                guard copy[key] as? String == receipt[key] as? String else { throw fail("Private copy failed verification.") }
            }
            let home = run.appendingPathComponent("home"), tmp = run.appendingPathComponent("tmp")
            let userData = run.appendingPathComponent("user-data")
            try preparePrivateHome(home, tmp: tmp, userData: userData, testDevice: testDevice, batchExport: headlessExport)
            let (sandbox, trampoline) = try prepareSandbox(run, source: source, normalData: normalData)
            var env = ProcessInfo.processInfo.environment.filter { !$0.key.hasPrefix("DYLD_") && !$0.key.hasPrefix("RYSUP_") && $0.key != "CFFIXED_USER_HOME" }
            env.merge(["RYSUP_FLARA_HELPER_PATH": helper.path, "RYSUP_FLARA_HELPER_MANIFEST": helper.deletingLastPathComponent().appendingPathComponent("helper.json").path,
                       "RYSUP_FLARA_SESSION_MANIFEST": manifest.path, "RYSUP_FLARA_EXPECTED_HELPER_SHA256": helperHash, "CFFIXED_USER_HOME": home.path, "TMPDIR": tmp.path,
                       "RYSUP_FLARA_RUN_ID": runID, "RYSUP_FLARA_EXPECTED_EXE": app.appendingPathComponent(executablePath).path,
                       "RYSUP_FLARA_EXPECTED_PARENT_PID": String(getpid()), "RYSUP_FLARA_RUN_DIR": run.path,
                       "RYSUP_FLARA_SOURCE_APP": source.path, "RYSUP_FLARA_ORIGINAL_REGISTRY_PATH": fm.homeDirectoryForCurrentUser.appendingPathComponent("Library/Preferences/Image-Line/reg.xml").path, "RYSUP_FLARA_PROJECT_PATH": project?.path ?? "",
                       "RYSUP_FLARA_MODE": testDevice == nil ? "normal" : "test"]) { _, new in new }
            if let testDevice { env["RYSUP_FLARA_TEST_AUDIO_DEVICE_UID"] = testDevice }
            var nativeArguments = project.map { [$0.path] } ?? []
            if headlessExport, let project {
                env["RYSUP_FLARA_BATCH_EXPORT"] = "1"
                env["RYSUP_FLARA_RATE_HOLD_PATH"] = rateHold.path
                env["RYSUP_ENGINE_RATE"] = "48000"
                env["RYSUP_RATE_HOLD_LOG"] = run.appendingPathComponent("rate-hold.log").path
                nativeArguments = ["-R", "-Ewav", project.path, "-O" + run.appendingPathComponent("export").path]
                receipt["headless_export_route"] = "normal_native_cli"
            }
            let consoleURL = run.appendingPathComponent("console.log"); fm.createFile(atPath: consoleURL.path, contents: nil, attributes: [.posixPermissions: 0o600])
            let console = try FileHandle(forWritingTo: consoleURL); defer { try? console.close() }
            let task = try OwnedChild(executable: URL(fileURLWithPath: "/usr/bin/sandbox-exec"),
                                      arguments: ["-f", sandbox.path, "/bin/sh", trampoline.path] + nativeArguments, environment: env, directory: run, console: console)
            process = task
            receipt["pid"] = task.processIdentifier; receipt["process_group"] = getpgid(task.processIdentifier); receipt["status"] = "starting"; try jsonWrite(receipt, manifest)
            let deadline = Date().addingTimeInterval(180)
            var ready = false
            while Date() < deadline {
                let duplicates = try liveExactProcesses(app.appendingPathComponent(executablePath)).filter { $0 != task.processIdentifier }
                guard duplicates.isEmpty else { throw fail("An unbridged duplicate private FL process appeared (PIDs \(duplicates)). No process was adopted or stopped; preserve this app and lock until all copies exit.") }
                let statusURL = run.appendingPathComponent("fl-ara-status.json")
                if let data = try? Data(contentsOf: statusURL), let status = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                   status["run_id"] as? String == runID, status["pid"] as? Int32 == task.processIdentifier {
                    if let error = status["error"] as? String, !error.isEmpty { throw fail("Native FL bridge refused: \(error). See \(run.path).") }
                    if status["runtime_helper_admitted"] as? Bool == true, status["native_engine_ready"] as? Bool == true { ready = true; break }
                }
                if !task.isRunning {
                    if headlessExport { break }
                    throw fail(startupExitMessage(task.exitStatusObserved ? task.terminationStatus : nil, diagnostics: run))
                }
                Thread.sleep(forTimeInterval: 0.2)
            }
            guard ready || (headlessExport && !task.isRunning) else { throw fail("Bridge startup was not confirmed. Any live FL Studio copy is preserved. See \(run.path).") }
            nativeStartupConfirmed = true
            receipt["status"] = "running"; try jsonWrite(receipt, manifest)
            changed("FL Studio is open in a private copy. Wait for ARA audio preparation before playback. Quit this private copy when finished.\nDiagnostics: \(run.path)")
            print("Private FL Studio PID \(task.processIdentifier). Diagnostics: \(run.path)")
            while task.isRunning {
                let duplicates = try liveExactProcesses(app.appendingPathComponent(executablePath)).filter { $0 != task.processIdentifier }
                guard duplicates.isEmpty else { throw fail("Additional private FL executables appeared (PIDs \(duplicates)); app copy and lock are preserved until all exit normally.") }
                if receipt["ara_runtime_compatible"] as? Bool != false,
                   let handle = try? FileHandle(forReadingFrom: log) {
                    let size = (try? handle.seekToEnd()) ?? 0
                    try? handle.seek(toOffset: size > 8192 ? size - 8192 : 0)
                    let tail = String(decoding: (try? handle.readToEnd()) ?? Data(), as: UTF8.self)
                    try? handle.close()
                    if tail.contains("bridge_runtime_degraded") {
                        receipt["ara_runtime_compatible"] = false
                        receipt["runtime_warning"] = "ARA compatibility check failed; save your work, remove the affected insert and restart before further ARA editing. Live FL Studio is preserved."
                        try jsonWrite(receipt, manifest)
                        changed("ARA compatibility check failed. FL Studio remains open so you can save your work. Follow its compatibility alert, then restart before further ARA editing.\nDiagnostics: \(run.path)")
                    }
                }
                Thread.sleep(forTimeInterval: 0.5)
            }
            if headlessExport {
                guard task.exitStatusObserved, task.terminationStatus == 0 else { throw fail("Native batch exited unsuccessfully; diagnostics and output are preserved.") }
                receipt["native_batch_output"] = try verifyNativeBatchOutput(run: run, runID: runID, pid: task.processIdentifier)
            }
            receipt["status"] = "exited"
        } catch {
            operationError = error; receipt["status"] = "failed"; receipt["error"] = error.localizedDescription
        }
        // Preserve every live child; a startup PID is never permission to kill a user session.
        let remaining = try? liveExactProcesses(app.appendingPathComponent(executablePath))
        let childRunning = process?.isRunning == true
        let cleanup = sessionCleanupDecision(childRunning: childRunning, exactPrivatePIDs: remaining)
        receipt["cleanup_decision"] = cleanup.rawValue
        if let task = process, !childRunning {
            receipt["exit_status_observed"] = task.exitStatusObserved
            receipt["exit_code"] = task.exitStatusObserved ? task.terminationStatus as Any : NSNull()
            if !nativeStartupConfirmed, task.exitStatusObserved, task.terminationStatus == 78 {
                receipt["status"] = "startup_refused"; receipt["startup_refused"] = true
                receipt["ara_capability_verdict"] = "not_established"
                receipt["startup_refusal_event"] = ownedStartupRefusalEvent(log: log, runID: runID, pid: task.processIdentifier) as Any? ?? NSNull()
                operationError = fail(startupExitMessage(78, diagnostics: run))
            }
        }
        let alive = cleanup != .removeExitedCopy
        receipt["remaining_private_pids"] = remaining as Any? ?? NSNull()
        if alive {
            receipt["status"] = "detached"; receipt["cleanup_pending"] = "At least one exact private executable is alive, or the inventory failed; copy and lock preserved."
            if operationError == nil { operationError = fail("Private FL executables remain alive; preserve the copy and lock until all exit normally.") }
        } else {
            if fm.fileExists(atPath: app.path), canonical(app) == app {
                unregister(app)
                do { try fm.removeItem(at: app) } catch { receipt["cleanup_error"] = error.localizedDescription; operationError = error }
            }
            if receipt["cleanup_error"] == nil {
                try? fm.removeItem(at: lock.appendingPathComponent("owner.json")); try? fm.removeItem(at: lock)
            }
        }
        do {
            let after = try inspect(source)
            receipt["source_unchanged"] = ["engine_sha256", "executable_sha256"].allSatisfy { after[$0] as? String == receipt[$0] as? String }
            if receipt["source_unchanged"] as? Bool != true { throw fail("Post-run source verification failed.") }
        } catch { receipt["source_unchanged"] = false; receipt["source_verification_error"] = error.localizedDescription; operationError = error }
        receipt.merge(handlerDecision(original: originalHandler, current: handlerSnapshot()).receipt) { _, new in new }
        try jsonWrite(receipt, manifest)
        if let operationError { throw operationError }
        changed("FL Studio exited. Private copy removed and original installation verified unchanged.\nDiagnostics: \(run.path)")
    }
}
func defaultCache() -> URL {
    if let path = ProcessInfo.processInfo.environment["RYSUP_FLARA_CACHE_ROOT"], path.hasPrefix("/") { return URL(fileURLWithPath: path) }
    return fm.homeDirectoryForCurrentUser.appendingPathComponent("Library/Caches/FLARALauncher")
}
func bundledHelper() -> URL { Bundle.main.resourceURL!.appendingPathComponent("native/FLAppBridge.dylib") }
final class LauncherDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
    var window: NSWindow!; var status: NSTextField!; var buttons: [NSButton] = []; var busy = false; var session: Session?
    var source = URL(fileURLWithPath: "/Applications/FL Studio 2026.app")
    func applicationDidFinishLaunching(_ notification: Notification) {
        let menu = NSMenu(); let appItem = NSMenuItem(); menu.addItem(appItem)
        let submenu = NSMenu(); submenu.addItem(withTitle: "Quit FL ARA Launcher", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q"); appItem.submenu = submenu; NSApp.mainMenu = menu
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 600, height: 380), styleMask: [.titled, .closable, .miniaturizable], backing: .buffered, defer: false)
        window.title = "FL ARA Launcher"; window.delegate = self; window.center(); window.isReleasedWhenClosed = false
        let stack = NSStackView(); stack.orientation = .vertical; stack.alignment = .leading; stack.spacing = 14; stack.translatesAutoresizingMaskIntoConstraints = false
        let title = NSTextField(labelWithString: "Native ARA 2 • FL Studio 26.1.7.5421"); title.font = .boldSystemFont(ofSize: 20); stack.addArrangedSubview(title)
        let intro = NSTextField(wrappingLabelWithString: "Experimental candidate. Launches an unchanged private copy of your licensed FL Studio. Use backed-up projects. Your installed FL Studio stays untouched. Third-party plug-in licenses are required."); stack.addArrangedSubview(intro)
        for (label, action) in [("Launch FL Studio", #selector(launchEmpty)), ("Open a backed-up project…", #selector(openProject)), ("Check compatibility…", #selector(check)), ("Choose FL Studio application…", #selector(chooseFL)), ("Clean finished session", #selector(cleanup))] {
            let button = NSButton(title: label, target: self, action: action); buttons.append(button); stack.addArrangedSubview(button)
        }
        status = NSTextField(wrappingLabelWithString: "Ready. Keep this launcher open while using its FL Studio copy."); status.font = .systemFont(ofSize: 12); stack.addArrangedSubview(status)
        window.contentView!.addSubview(stack); NSLayoutConstraint.activate([stack.leadingAnchor.constraint(equalTo: window.contentView!.leadingAnchor, constant: 24), stack.trailingAnchor.constraint(equalTo: window.contentView!.trailingAnchor, constant: -24), stack.topAnchor.constraint(equalTo: window.contentView!.topAnchor, constant: 24)])
        window.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps: true)
    }
    func alert(_ text: String) { let alert = NSAlert(); alert.messageText = "FL ARA Launcher"; alert.informativeText = text; alert.runModal() }
    @objc func chooseFL() { let panel = NSOpenPanel(); panel.title = "Select your licensed FL Studio application"; panel.canChooseFiles = true; panel.canChooseDirectories = false; panel.allowedContentTypes = [.applicationBundle]; if panel.runModal() == .OK, let url = panel.url { source = canonical(url); status.stringValue = "Selected: \(source.path)" } }
    func work(_ block: @escaping () throws -> String) {
        guard !busy else { return }; busy = true; buttons.forEach { $0.isEnabled = false }
        DispatchQueue.global(qos: .userInitiated).async {
            var message: String
            do { message = try block() } catch { message = error.localizedDescription }
            DispatchQueue.main.async { self.status.stringValue = message; self.busy = false; self.buttons.forEach { $0.isEnabled = true } }
        }
    }
    @objc func check() { work { _ = try inspect(self.source); return "Reference verified: FL Studio 26.1.7.5421, exact hashes and original vendor signature. Runtime ARA qualification remains pending." } }
    @objc func cleanup() { work { try cleanupFinishedSession(defaultCache()) } }
    @objc func launchEmpty() { start(nil) }
    @objc func openProject() {
        let panel = NSOpenPanel(); panel.title = "Select a backed-up .flp project"; panel.canChooseDirectories = true; panel.canChooseFiles = true; panel.treatsFilePackagesAsDirectories = false
        if panel.runModal() == .OK, let url = panel.url { guard url.pathExtension == "flp" else { alert("Select a .flp project."); return }; start(canonical(url)) }
    }
    func start(_ project: URL?) {
        guard fm.fileExists(atPath: source.path) else { chooseFL(); return }
        let session = Session(cache: defaultCache(), helper: bundledHelper()); self.session = session
        session.changed = { [weak self] text in DispatchQueue.main.async { self?.status.stringValue = text } }
        work { try session.launch(source: self.source, project: project); return self.statusText() }
    }
    func statusText() -> String { "FL Studio exited. Private copy removed and original installation verified unchanged. Diagnostics: \(defaultCache().path)." }
    func windowShouldClose(_ sender: NSWindow) -> Bool { NSApp.terminate(nil); return false }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        if busy {
            let alert = NSAlert(); alert.messageText = "Keep the launcher running?"; alert.informativeText = "Leaving now preserves any running FL Studio copy and its session lock. Quit that FL Studio copy normally, then use Clean finished session next time."; alert.addButton(withTitle: "Keep launcher running"); alert.addButton(withTitle: "Leave FL Studio open and quit launcher")
            return alert.runModal() == .alertFirstButtonReturn ? .terminateCancel : .terminateNow
        }
        return .terminateNow
    }
}
func selfTest() throws {
    let base = fm.temporaryDirectory.appendingPathComponent("fl-ara-launcher-test-" + UUID().uuidString)
    try fm.createDirectory(at: base, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
    defer { try? fm.removeItem(at: base) }
    let boundary = "/" + String(repeating: "a", count: 151)
    try validatePrivateAppPath(boundary) // 152 + 103 == 255, inclusive.
    let nonBMPBoundary = "/" + String(repeating: "a", count: 149) + "😀"
    guard nonBMPBoundary.utf16.count == 152, nonBMPBoundary.count == 151 else { throw fail("UTF-16 boundary fixture is invalid.") }
    try validatePrivateAppPath(nonBMPBoundary)
    for path in [boundary + "a", nonBMPBoundary + "a"] {
        do { try validatePrivateAppPath(path); throw fail("Overlong template path admitted.") }
        catch let error as LaunchError { guard error.message.contains("256/255 UTF-16") else { throw error } }
    }
    var refusedCopy = base
    while physicalPath(refusedCopy).utf16.count <= 152 { refusedCopy.appendPathComponent("long-cache-component") }
    refusedCopy.appendPathComponent("FL Studio 2026.app")
    do {
        _ = try copyPrivateApp(base.appendingPathComponent("missing-source"), to: refusedCopy, cache: base.appendingPathComponent("missing-cache"))
        throw fail("Overlong private copy was attempted.")
    } catch let error as LaunchError {
        guard error.message.contains("shorter real --cache-root"), !fm.fileExists(atPath: refusedCopy.path),
              !fm.fileExists(atPath: base.appendingPathComponent("missing-source").path) else { throw error }
    }
    let home = base.appendingPathComponent("home"), tmp = base.appendingPathComponent("tmp"), data = base.appendingPathComponent("data")
    try preparePrivateHome(home, tmp: tmp, userData: data)
    let xml = try String(contentsOf: home.appendingPathComponent("Library/Preferences/Image-Line/reg.xml"), encoding: .utf8)
    guard xml.contains(xmlEscape(data.path + "/")), !xml.contains("Registration"), !xml.contains("License"), !xml.contains("Device name"), !xml.contains("Plugin version specific") else { throw fail("Private registry test failed.") }
    let document = try XMLDocument(xmlString: xml)
    let rates = try document.nodes(forXPath: "//Key[@Name='Image-Line']/Key[@Name='FL Studio 26']/Key[@Name='Devices']/Key[@Name='Audio output']/Value[@Name='Sample rate' and @Type='2']")
    guard rates.count == 1, rates[0].stringValue == "48000" else { throw fail("Normal-app rate is not the exact FL26 normal Devices/Audio output registry setting.") }
    try preparePrivateHome(home, tmp: tmp, userData: data, testDevice: "Pro Tools Audio Bridge 2-A_UID")
    let testXML = try String(contentsOf: home.appendingPathComponent("Library/Preferences/Image-Line/reg.xml"), encoding: .utf8)
    let testDocument = try XMLDocument(xmlString: testXML)
    let devices = try testDocument.nodes(forXPath: "//Key[@Name='Image-Line']/Key[@Name='FL Studio 26']/Key[@Name='Devices']/Key[@Name='Audio output']/Value[@Name='Device name' and @Type='2']")
    guard devices.count == 1, devices[0].stringValue == "4Pro Tools Audio Bridge 2-A" else { throw fail("Test-only native device registry setting is incorrect.") }
    let own = try liveExactProcesses(URL(fileURLWithPath: CommandLine.arguments[0]))
    guard own.contains(getpid()) else { throw fail("Exact executable inventory failed to find this process.") }
    let privateExe = physicalPath(base.appendingPathComponent("owned-a/FL Studio 2026.app/Contents/MacOS/OsxFL"))
    let independentInstances: [(pid: Int32, path: String)] = [
        (101, "/Applications/FL Studio 2026.app/Contents/MacOS/OsxFL"),
        (102, physicalPath(base.appendingPathComponent("owned-b/FL Studio 2026.app/Contents/MacOS/OsxFL"))),
        (103, privateExe + "-other")
    ]
    let unrelated = exactPrivateProcessPIDs(independentInstances, expectedPhysicalPath: privateExe)
    let oneOwned = exactPrivateProcessPIDs(independentInstances + [(104, privateExe)], expectedPhysicalPath: privateExe)
    let duplicated = exactPrivateProcessPIDs(independentInstances + [(104, privateExe), (105, privateExe)], expectedPhysicalPath: privateExe)
    guard unrelated.isEmpty, oneOwned == [104], oneOwned.filter({ $0 != 104 }).isEmpty,
          duplicated.filter({ $0 != 104 }) == [105],
          sessionCleanupDecision(childRunning: false, exactPrivatePIDs: unrelated) == .removeExitedCopy,
          sessionCleanupDecision(childRunning: false, exactPrivatePIDs: duplicated) == .preserveExactPrivateProcesses else {
        throw fail("Independent FL instances were blocked or exact-copy duplicate protection failed.")
    }
    let link = base.appendingPathComponent("escape")
    try fm.createSymbolicLink(at: link, withDestinationURL: base)
    do { try rejectSymlinkAncestors(link.appendingPathComponent("nested")); throw fail("Symlink admitted.") }
    catch let error as LaunchError { guard error.message.contains("symlinks") else { throw error } }
    let source = base.appendingPathComponent("Fake FL.app")
    try fm.createDirectory(at: source, withIntermediateDirectories: false)
    let protectedFile = source.appendingPathComponent("test.txt")
    try Data("original".utf8).write(to: protectedFile)
    let (sandbox, trampoline) = try prepareSandbox(base, source: source, normalData: data)
    let write = "printf changed > \"$1\""
    do { _ = try command("/usr/bin/sandbox-exec", ["-f", sandbox.path, "/bin/sh", "-c", write, "test", protectedFile.path]); throw fail("Sandbox allowed original-app write.") }
    catch let error as LaunchError { guard error.message.contains("sandbox-exec failed") else { throw error } }
    guard try String(contentsOf: protectedFile, encoding: .utf8) == "original" else { throw fail("Sandbox preservation test failed.") }
    let allowed = base.appendingPathComponent("allowed.txt")
    _ = try command("/usr/bin/sandbox-exec", ["-f", sandbox.path, "/bin/sh", "-c", write, "test", allowed.path])
    guard fm.fileExists(atPath: allowed.path), try String(contentsOf: trampoline, encoding: .utf8).contains("export DYLD_INSERT_LIBRARIES=\"$RYSUP_FLARA_HELPER_PATH\"") else { throw fail("Sandbox allowed-write or trampoline test failed.") }
    // Exercise an actual owned child/refusal status, without loading FL or a
    // plugin. Unknown/duplicate inventory must never authorize copy removal.
    let consoleURL = base.appendingPathComponent("exit78.log")
    fm.createFile(atPath: consoleURL.path, contents: nil, attributes: [.posixPermissions: 0o600])
    let console = try FileHandle(forWritingTo: consoleURL); defer { try? console.close() }
    let child = try OwnedChild(executable: URL(fileURLWithPath: "/bin/sh"), arguments: ["-c", "exit 78"],
                               environment: ["PATH": "/usr/bin:/bin", "HOME": base.path, "CFFIXED_USER_HOME": base.path, "TMPDIR": base.path], directory: base, console: console)
    child.waitUntilExit()
    guard child.exitStatusObserved, child.terminationStatus == 78,
          startupExitMessage(78, diagnostics: base).contains("refused startup"),
          !startupExitMessage(1, diagnostics: base).contains("refused startup"),
          sessionCleanupDecision(childRunning: true, exactPrivatePIDs: []) == .preserveLiveChild,
          sessionCleanupDecision(childRunning: false, exactPrivatePIDs: [12345]) == .preserveExactPrivateProcesses,
          sessionCleanupDecision(childRunning: false, exactPrivatePIDs: nil) == .preserveUnknownProcessInventory,
          sessionCleanupDecision(childRunning: false, exactPrivatePIDs: []) == .removeExitedCopy else {
        throw fail("Owned exit-78 observation or cleanup-preservation decision failed.")
    }
    let events = base.appendingPathComponent("events.jsonl")
    try Data(("{\"event\":\"owned_startup_save_guard_refused\",\"run_id\":\"another-run\",\"pid\":123}\n" +
              "{\"event\":\"owned_startup_engine_identity_refused\",\"run_id\":\"test-run\",\"pid\":456}\n" +
              "{\"event\":\"owned_startup_create_contract_refused\",\"run_id\":\"test-run\",\"pid\":789}\n").utf8).write(to: events)
    guard ownedStartupRefusalEvent(log: events, runID: "test-run", pid: 456) == "owned_startup_engine_identity_refused",
          ownedStartupRefusalEvent(log: events, runID: "test-run", pid: 789) == "owned_startup_create_contract_refused",
          ownedStartupRefusalEvent(log: events, runID: "test-run", pid: 123) == nil else {
        throw fail("Startup refusal event admitted another run or process.")
    }
    let vendor = "com.image-line.flstudio"
    let unchanged = handlerDecision(original: vendor, current: vendor)
    let userChanged = handlerDecision(original: vendor, current: "com.user.chosen-editor")
    let notOurs = handlerDecision(original: "com.user.chosen-editor", current: "com.another.editor")
    let unknown = handlerDecision(original: nil, current: nil)
    guard !unchanged.externalChange, userChanged.externalChange, notOurs.externalChange,
          ![unchanged,userChanged,notOurs,unknown].contains(where: { $0.defaultWriteAuthorized }) else {
        throw fail("Default-handler preservation decision authorized a write or misclassified a user choice.")
    }
    print("PASS UTF-16 template path boundary/non-BMP/refuse-before-copy; private home; normal48k+test-only native device registry; exact process inventory; independent installed/owned instances allowed; exact-copy duplicates preserved; no license duplication; symlink refusal; denied source write; allowed owned-run write; SIP-safe helper trampoline; read-only default-handler decisions; actual owned exit78 and conservative cleanup controls. No FL process launched.")
}
var args = Array(CommandLine.arguments.dropFirst())
if args.isEmpty || args == ["--gui"] {
    let app = NSApplication.shared; let delegate = LauncherDelegate(); app.delegate = delegate; app.setActivationPolicy(.regular)
    withExtendedLifetime(delegate) { app.run() }
} else {
    do {
        var source = URL(fileURLWithPath: "/Applications/FL Studio 2026.app"), project: URL?, cache = defaultCache(); var check = false, cleanup = false, headlessExport = false; var testDevice: String?
        while !args.isEmpty {
            let arg = args.removeFirst()
            switch arg {
            case "--test-device":
                guard !args.isEmpty else { throw fail("--test-device requires a UID.") }; testDevice = args.removeFirst()
            case "--headless-export": headlessExport = true
            case "--self-test": try selfTest(); exit(0)
            case "--version": print(launcherVersion); exit(0)
            case "--check": check = true
            case "--cleanup": cleanup = true
            case "--fl", "--cache", "--cache-root":
                guard !args.isEmpty else { throw fail("\(arg) requires a path.") }; let path = URL(fileURLWithPath: args.removeFirst()); if arg == "--fl" { source = path } else { cache = path }
            default: guard !arg.hasPrefix("-"), project == nil else { throw fail("Unknown argument: \(arg)") }; project = canonical(URL(fileURLWithPath: arg))
            }
        }
        if check { let data = try JSONSerialization.data(withJSONObject: inspect(source), options: [.prettyPrinted, .sortedKeys]); print(String(decoding: data, as: UTF8.self)) }
        else if cleanup { print(try cleanupFinishedSession(cache.standardizedFileURL)) }
        else { try Session(cache: cache, helper: bundledHelper()).launch(source: source, project: project, testDevice: testDevice, headlessExport: headlessExport) }
    } catch { fputs("FL ARA Launcher: \(error.localizedDescription)\n", stderr); exit(1) }
}
