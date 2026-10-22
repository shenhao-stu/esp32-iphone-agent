import Foundation
import UIKit

/// What the agent and the pointer need from the screen (VideoFeed, or a fake in tests). Port of screen.py.
@MainActor protocol ScreenSource: AnyObject {
    var live: Bool { get }
    var frame: Data? { get }
    var updated: Double { get }
    var capturedAt: Double { get }
    var aspect: Double { get }
    /// After a gesture that ended at `since`: wait until the screen shows its result (and fetch it, in snap mode).
    func afterAction(since: Double) async
}

/// How a run gets its frames: the live stream (`stream on`, the default: a frame in under a second), or one
/// snap after each action (`stream off`; waits for the board's whole GOP, often 20-30 s).
enum FrameMode: String {
    case stream, snap
    static let key = "frameModeV2"  // V2: drops the old saved "snap" left by earlier automatic fallbacks
    /// A command timeout switches the current run to snap; not saved, so the next run streams again.
    static var fallback: FrameMode?
    static var current: FrameMode {
        get { fallback ?? FrameMode(rawValue: UserDefaults.standard.string(forKey: key) ?? "") ?? .stream }
        set { UserDefaults.standard.set(newValue.rawValue, forKey: key); fallback = nil }
    }
}

extension ScreenSource {
    func afterAction(since: Double) async { await settled() }

    /// Wait until the screen stops changing (AirPlay only sends frames on change). Mid-animation frames are big and
    /// can reach us late over BLE, so a gap is only "quiet" once it also covers the last frame's transport lag.
    func settled(quiet: Double = 0.5, least: Double = 0.3, most: Double = 4) async {
        let started = now()
        try? await Task.sleep(nanoseconds: UInt64(least * 1e9))
        while now() - updated < quiet + lag && now() - started < most {
            try? await Task.sleep(nanoseconds: 100_000_000)
        }
    }

    /// How late the shown frame arrived after the board got it (0 when unknown), capped.
    private var lag: Double { capturedAt > 0 ? min(max(updated - capturedAt, 0), 1) : 0 }

    /// The first frame captured after `since` (received, when capture times are unknown), or nil if none in time.
    func fresh(since: Double, timeout: Double = 0.8) async -> Data? {
        let deadline = now() + timeout
        while (capturedAt > 0 ? capturedAt : updated) <= since {
            if now() > deadline { return nil }
            try? await Task.sleep(nanoseconds: 20_000_000)
        }
        return frame
    }
}

/// The phone's screen, mirrored over AirPlay to the board and forwarded over BLE, as JPEG frames.
@MainActor final class VideoFeed: ObservableObject, ScreenSource {
    @Published private(set) var frame: Data?
    @Published private(set) var size = CGSize.zero
    @Published private(set) var note = ""  // last decoder problem, shown in Diagnostics
    private(set) var updated = 0.0
    /// When the board received the shown frame, in local time (≈; see `boardClock`).
    private(set) var capturedAt = 0.0
    /// A decode error or a lost chunk since the last good frame: the shown frame is stale until the next IDR.
    private(set) var broken = false
    private var clockOffset: Double?  // local s − board s, the smallest seen (≈ the link latency)
    var mirroring = false  // board status says ap == streaming
    var metrics: Metrics?
    weak var device: HIDDevice?

    private var assembler = VideoAssembler()
    private let decoder = H264Decoder()

    init() {
        decoder.onFrame = { [weak self] f in DispatchQueue.main.async { self?.publish(f) } }
        decoder.onError = { [weak self] msg in
            DispatchQueue.main.async {
                let bg = UIApplication.shared.applicationState == .background
                self?.note = bg ? "\(msg) [in background]" : msg
                self?.metrics?.decodeError(background: bg)
                self?.broken = true
            }
        }
    }

    var live: Bool { mirroring && frame != nil && (device?.connected ?? false) }

    /// Frame width / height (portrait iPhone as a fallback).
    var aspect: Double { size.width > 0 && size.height > 0 ? size.width / size.height : 9 / 19.5 }

    /// One `video` notify from BoardLink.
    func receive(_ chunk: Data) {
        metrics?.videoBytes(chunk.count)
        let dropsBefore = assembler.drops
        let unit = assembler.add(chunk)
        if assembler.drops > dropsBefore { metrics?.videoDrops(assembler.drops - dropsBefore); broken = true }
        guard let unit else { return }
        if !unit.snap { boardClock(ms: Int(unit.boardMs)) }  // snap units are resent late: their ages say nothing
        if unit.idr { metrics?.idrSeen() }
        decoder.queue.async { [decoder] in decoder.decode(unit) }
    }

    /// The link (re)connected: chunk numbering starts over.
    func resetStream() { assembler.reset(); clockOffset = nil }  // the board may have rebooted: its clock too

    /// A board timestamp (status `up`, or a streamed unit) just arrived: keep the smallest offset.
    func boardClock(ms: Int) {
        let offset = now() - Double(ms) / 1000
        clockOffset = min(clockOffset ?? offset, offset)
    }

    /// Show a still frame (demo data).
    func show(_ jpeg: Data, width: Int, height: Int) {
        publish(H264Decoder.Frame(jpeg: jpeg, width: width, height: height, decodeMs: 4.2,
                                  unit: AccessUnit(data: Data(), idr: false, config: false, snap: false, boardMs: 0)))
    }

    private func publish(_ f: H264Decoder.Frame) {
        frame = f.jpeg
        updated = now()
        capturedAt = clockOffset.map { Double(f.unit.boardMs) / 1000 + $0 } ?? updated
        broken = false
        if size.width != CGFloat(f.width) || size.height != CGFloat(f.height) {
            size = CGSize(width: f.width, height: f.height)
        }
        metrics?.frameDecoded(ms: f.decodeMs)
    }

    /// The app asked the board to forward the stream (`stream on`).
    private(set) var streamOn = false

    /// iOS sends an IDR only when mirroring starts, so a frame can only be decoded from every unit since then:
    /// in stream mode the stream stays on for as long as the board mirrors, not just during runs.
    var keepStreaming: Bool { FrameMode.current == .stream && !MockMode.on }

    /// Done with the screen for now: stop the stream unless it stays on (see `keepStreaming`).
    func release() async throws {
        if streamOn && !keepStreaming { try await stream(false) }
    }

    /// Seconds a snap may take: the buffered GOP over the measured BLE rate, doubled, at least 8 s.
    var snapTimeout: Double {
        let gopBytes = Double(metrics?.board?.gop ?? 0)
        let kbps = max(metrics?.bleKbps ?? 0, metrics?.board?.ble?.kbps ?? 0, 40)
        return max(8, gopBytes * 8 / 1000 / kbps * 2 + 3)
    }

    /// A current frame for the model. While the board streams, the live stream has it: use a frame younger than
    /// `maxAge`, or wait for the next one. AirPlay sends frames only on change, so a healthy stream with nothing new
    /// while the board gets nothing either means the screen is unchanged. Snap only as a fallback (it resends the
    /// whole GOP since mirroring started), retried once with a longer timeout. Returns where the frame came from.
    @discardableResult
    func currentFrame(maxAge: Double = 1, wait: Double = 3, catchUp: Double = 180) async throws -> String {
        if streamOn && mirroring {
            if frame != nil && now() - updated < maxAge { return "live" }
            if await fresh(since: now() - maxAge, timeout: wait) != nil { return "live (waited)" }
            if frame != nil && !broken {
                // The board reports how far behind our stream is: everything it has will reach us, while a snap would
                // queue the whole GOP behind that backlog (seen: 117 s, and the phone auto-locked meanwhile).
                if let lag = metrics?.board?.lag, lag >= 0 {
                    if lag > Self.caughtUpBytes {
                        DebugLog.write("frame: stream backlog \(lag / 1024) KB; waiting for it instead of snapping")
                        guard await caughtUp(timeout: catchUp) else {
                            throw DeviceError(message: "the screen stream did not catch up in \(Int(catchUp)) s")
                        }
                        await decoded()
                        return "live (caught up)"
                    }
                    await decoded()
                    return "live (unchanged)"
                }
                if (metrics?.board?.fps ?? 1) < 0.5 { return "live (unchanged)" }
            }
        }
        do {
            try await snap()
        } catch is BoardTimeout {
            DebugLog.write("snap timed out; retrying once with a longer timeout")
            try await snap(timeout: snapTimeout * 1.5)
        }
        return "snap"
    }

    /// A backlog this small is a frame or two in flight, not a lag worth waiting for.
    static let caughtUpBytes = 8 * 1024

    /// Waits until the board has (almost) nothing left to stream to us, or a frame captured after we started waiting
    /// arrives (backlog frames carry their old capture times); false on timeout or when the stream breaks, stops or
    /// mirroring ends.
    private func caughtUp(timeout: Double) async -> Bool {
        let started = now(), deadline = started + timeout
        while now() < deadline {
            if let lag = metrics?.board?.lag, lag >= 0, lag <= Self.caughtUpBytes { return true }
            if capturedAt > started { return true }
            if broken || !streamOn || !mirroring { return false }
            try? await Task.sleep(nanoseconds: 250_000_000)
        }
        return false
    }

    /// Lets the decoder finish what it has received, so `frame` is the newest unit.
    private func decoded() async {
        await withCheckedContinuation { c in decoder.queue.async { c.resume() } }
        try? await Task.sleep(nanoseconds: 30_000_000)  // the last frame reaches the main queue
    }

    /// Ask the board for the current GOP and wait until it is decoded. Returns the latency in ms.
    @discardableResult
    func snap(timeout: Double? = nil) async throws -> Double {
        guard let device else { throw DeviceError(message: "board not connected") }
        let started = now()
        if let channel = device as? CommandChannel {
            try await channel.send("snap", timeout: timeout ?? snapTimeout)
        } else {
            try await device.send("snap")
        }
        await decoded()  // the GOP is decoded
        let ms = (now() - started) * 1000
        metrics?.snapLatency(ms)
        return ms
    }

    func stream(_ on: Bool) async throws {
        try await device?.send("stream \(on ? "on" : "off")")
        streamOn = on
    }

    /// The model must never see a frame from before the action that ended at `since`. Stream: let the screen
    /// settle, then wait (bounded) for a frame captured after the action; none while the stream is healthy means
    /// the screen did not change, but a broken stream (waiting for an IDR) needs a snap. Snap: give the UI a moment,
    /// snap, and snap once more if the GOP still ends before the action.
    func afterAction(since: Double) async {
        do {
            if FrameMode.current == .snap {
                try await Task.sleep(nanoseconds: 700_000_000)
                try await currentFrame()
                guard capturedAt < since else { return }
                DebugLog.write(String(format: "frame: snap ends %.1f s before the action; snapping again", since - capturedAt))
                try await Task.sleep(nanoseconds: 500_000_000)
                try await currentFrame()
            } else {
                await settled()
                if await newer(than: since, timeout: 1.5) { return }
                if let lag = metrics?.board?.lag, lag > Self.caughtUpBytes, !broken, streamOn {
                    // the action's frames are queued behind a backlog: "unchanged" would show the model a stale screen
                    DebugLog.write("frame: stream backlog \(lag / 1024) KB after the action; waiting for it")
                    if await caughtUp(timeout: 180), await newer(than: since, timeout: 1.5) { return }
                }
                guard broken || !streamOn else { return DebugLog.write("frame: none after the action; screen unchanged") }
                DebugLog.write("frame: stream stale (waiting for an IDR); snapping")
                try await snap()
            }
        } catch {
            DebugLog.write("frame after action failed: \(error.localizedDescription)")
        }
        if capturedAt < since { DebugLog.write(String(format: "frame: still %.1f s older than the action", since - capturedAt)) }
    }

    /// Waits until the shown frame was captured after `t`; false on timeout.
    private func newer(than t: Double, timeout: Double) async -> Bool {
        let deadline = now() + timeout
        while capturedAt < t {
            if now() > deadline { return false }
            try? await Task.sleep(nanoseconds: 50_000_000)
        }
        return true
    }

    /// The link dropped: the board forgets `stream on`.
    func linkLost() { streamOn = false }
}
