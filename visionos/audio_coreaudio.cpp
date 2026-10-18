// Core Audio output for the portable mix in audio.cpp, on visionOS: a RemoteIO unit pulling
// 48 kHz stereo 16-bit, which is what the mix produces. The desktop uses audio_sdl.cpp and
// Android audio_aaudio.cpp.
#include "runtime.h"
#include <AudioToolbox/AudioToolbox.h>
#include <cstdio>

void audio_render(int16_t* out, int frames);
void audio_open_wav();

static constexpr double OUT_RATE = 48000.0;
static AudioComponentInstance g_unit;

static OSStatus on_render(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames,
                          AudioBufferList* io) {
    audio_render((int16_t*)io->mBuffers[0].mData, (int)frames);
    return noErr;
}

bool audio_open() {
    audio_open_wav();
    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_RemoteIO;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp || AudioComponentInstanceNew(comp, &g_unit) != noErr) {
        fprintf(stderr, "audio: no output unit\n");
        return false;
    }
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate = OUT_RATE;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    fmt.mChannelsPerFrame = 2;
    fmt.mBitsPerChannel = 16;
    fmt.mBytesPerFrame = 4;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerPacket = 4;
    OSStatus r = AudioUnitSetProperty(g_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt,
                                      sizeof(fmt));
    AURenderCallbackStruct cb{on_render, nullptr};
    if (r == noErr)
        r = AudioUnitSetProperty(g_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb,
                                 sizeof(cb));
    if (r == noErr) r = AudioUnitInitialize(g_unit);
    if (r == noErr) r = AudioOutputUnitStart(g_unit);
    if (r != noErr) {
        fprintf(stderr, "audio: cannot start the output unit (%d)\n", (int)r);
        return false;
    }
    fprintf(stderr, "audio: playing, 48 kHz stereo\n");
    return true;
}
