import Foundation
import ScreenCaptureKit
import CoreMedia
import AudioToolbox

@available(macOS 13.0, *)
final class KariPomAudioOutput: NSObject, SCStreamOutput {
    private let stdout = FileHandle.standardOutput

    func stream(_ stream: SCStream,
                didOutputSampleBuffer sampleBuffer: CMSampleBuffer,
                of outputType: SCStreamOutputType) {
        guard outputType == .audio,
              CMSampleBufferIsValid(sampleBuffer),
              CMSampleBufferGetNumSamples(sampleBuffer) > 0,
              let formatDescription = CMSampleBufferGetFormatDescription(sampleBuffer),
              let asbdPtr = CMAudioFormatDescriptionGetStreamBasicDescription(formatDescription)
        else { return }

        let asbd = asbdPtr.pointee
        guard asbd.mFormatID == kAudioFormatLinearPCM,
              (asbd.mFormatFlags & kAudioFormatFlagIsFloat) != 0,
              asbd.mBitsPerChannel == 32
        else {
            fputs("UNSUPPORTED_AUDIO_FORMAT\n", stderr)
            return
        }

        var blockBuffer: CMBlockBuffer?
        var audioBufferList = AudioBufferList(
            mNumberBuffers: 1,
            mBuffers: AudioBuffer(mNumberChannels: 1, mDataByteSize: 0, mData: nil)
        )
        let status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sampleBuffer,
            bufferListSizeNeededOut: nil,
            bufferListOut: &audioBufferList,
            bufferListSize: MemoryLayout<AudioBufferList>.size,
            blockBufferAllocator: kCFAllocatorDefault,
            blockBufferMemoryAllocator: kCFAllocatorDefault,
            flags: UInt32(kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment),
            blockBufferOut: &blockBuffer
        )
        guard status == noErr,
              audioBufferList.mNumberBuffers >= 1,
              let data = audioBufferList.mBuffers.mData
        else { return }
        stdout.write(Data(bytes: data, count: Int(audioBufferList.mBuffers.mDataByteSize)))
    }
}

@available(macOS 13.0, *)
final class KariPomCaptureDelegate: NSObject, SCStreamDelegate {
    func stream(_ stream: SCStream, didStopWithError error: Error) {
        fputs("STREAM_STOPPED: \(error.localizedDescription)\n", stderr)
        fflush(stderr)
        exit(3)
    }
}

@available(macOS 13.0, *)
func startCapture() async throws -> (SCStream, KariPomAudioOutput, KariPomCaptureDelegate) {
    let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
    guard let display = content.displays.first else {
        throw NSError(domain: "KariPomScreenCapture", code: 1,
                      userInfo: [NSLocalizedDescriptionKey: "No display is available for capture."])
    }

    let filter = SCContentFilter(display: display, excludingApplications: [], exceptingWindows: [])
    let config = SCStreamConfiguration()
    config.capturesAudio = true
    config.sampleRate = 44100
    config.channelCount = 1
    config.excludesCurrentProcessAudio = true
    // 画面フレームは受け取らないが、最小構成にしてScreenCaptureKitの負荷を抑える。
    config.width = 2
    config.height = 2
    config.minimumFrameInterval = CMTime(value: 1, timescale: 1)
    config.queueDepth = 1

    let output = KariPomAudioOutput()
    let delegate = KariPomCaptureDelegate()
    let stream = SCStream(filter: filter, configuration: config, delegate: delegate)
    let queue = DispatchQueue(label: "jp.karipom.desktop.audio")
    try stream.addStreamOutput(output, type: .audio, sampleHandlerQueue: queue)
    try await stream.startCapture()
    fputs("READY\n", stderr)
    fflush(stderr)
    return (stream, output, delegate)
}

if #available(macOS 13.0, *) {
    var retained: (SCStream, KariPomAudioOutput, KariPomCaptureDelegate)?
    Task {
        do {
            retained = try await startCapture()
        } catch {
            let ns = error as NSError
            fputs("START_ERROR: domain=\(ns.domain) code=\(ns.code) \(error.localizedDescription)\n", stderr)
            fflush(stderr)
            exit(2)
        }
    }
    RunLoop.main.run()
} else {
    fputs("UNSUPPORTED_MACOS: macOS 13 or later is required.\n", stderr)
    exit(4)
}
