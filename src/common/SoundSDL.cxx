//============================================================================
//
//   SSSS    tt          lll  lll
//  SS  SS   tt           ll   ll
//  SS     tttttt  eeee   ll   ll   aaaa
//   SSSS    tt   ee  ee  ll   ll      aa
//      SS   tt   eeeeee  ll   ll   aaaaa  --  "An Atari 2600 VCS Emulator"
//  SS  SS   tt   ee      ll   ll  aa  aa
//   SSSS     ttt  eeeee llll llll  aaaaa
//
// Copyright (c) 1995-2025 by Bradford W. Mott, Stephen Anthony
// and the Stella Team
//
// See the file "License.txt" for information on usage and redistribution of
// this file, and for a DISCLAIMER OF ALL WARRANTIES.
//============================================================================

#ifdef SOUND_SUPPORT

#include <cmath>

#include "SDL_lib.hxx"
#include "Logger.hxx"
#include "FrameBuffer.hxx"
#include "OSystem.hxx"
#include "Console.hxx"
#include "AudioQueue.hxx"
#include "EmulationTiming.hxx"
#include "AudioSettings.hxx"
#include "audio/SimpleResampler.hxx"
#include "audio/LanczosResampler.hxx"
#include "ThreadDebugging.hxx"

#include "SoundSDL.hxx"

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
SoundSDL::SoundSDL(OSystem& osystem, AudioSettings& audioSettings)
  : Sound{osystem},
    myAudioSettings{audioSettings}
{
  ASSERT_MAIN_THREAD;

  Logger::debug("SoundSDL::SoundSDL started ...");

  if(SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
  {
    ostringstream buf;

    buf << "WARNING: Failed to initialize SDL audio system! \n"
        << "         " << SDL_GetError() << '\n';
    Logger::error(buf.view());
    return;
  }

  queryHardware(myDevices);  // NOLINT

  SDL_zero(myHardwareSpec);
  if(!openDevice())
    return;

  Logger::debug("SoundSDL::SoundSDL initialized");
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
SoundSDL::~SoundSDL()
{
  ASSERT_MAIN_THREAD;

  if(!myIsInitializedFlag)
    return;

  SDL_CloseAudioDevice(myDevice);
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void SoundSDL::queryHardware(VariantList& devices)
{
  ASSERT_MAIN_THREAD;

  const int numDevices = SDL_GetNumAudioDevices(0);

  // log the available audio devices
  ostringstream s;
  s << "Supported audio devices (" << numDevices << "):";
  Logger::debug(s.view());

  VarList::push_back(devices, "Default", 0);
  for(int i = 0; i < numDevices; ++i)
  {
    ostringstream ss;

    ss << "  " << i + 1 << ": " << SDL_GetAudioDeviceName(i, 0);
    Logger::debug(ss.view());

    VarList::push_back(devices, SDL_GetAudioDeviceName(i, 0), i + 1);
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool SoundSDL::openDevice()
{
  ASSERT_MAIN_THREAD;

  SDL_AudioSpec desired;
  desired.freq   = myAudioSettings.sampleRate();
  desired.format = AUDIO_F32SYS;
  desired.channels = 2;
  desired.samples  = static_cast<Uint16>(myAudioSettings.fragmentSize());
  desired.callback = callback;
  desired.userdata = this;

  if(myIsInitializedFlag)
    SDL_CloseAudioDevice(myDevice);

  myDeviceId = BSPF::clamp(myAudioSettings.device(), 0U,
                           static_cast<uInt32>(myDevices.size() - 1));
  const char* const device = myDeviceId
    ? myDevices.at(myDeviceId).first.c_str()
    : nullptr;

  myDevice = SDL_OpenAudioDevice(device, 0, &desired, &myHardwareSpec,
                                 SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);

  if(myDevice == 0)
  {
    ostringstream buf;

    buf << "WARNING: Couldn't open SDL audio device! \n"
        << "         " << SDL_GetError() << '\n';
    Logger::error(buf.view());

    return myIsInitializedFlag = false;
  }
  return myIsInitializedFlag = true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void SoundSDL::setEnabled(bool enable)
{
  mute(!enable);
  pause(!enable);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void SoundSDL::open(shared_ptr<AudioQueue> audioQueue,
                    shared_ptr<const EmulationTiming> emulationTiming)
{
  const string pre_about = myAboutString;

  // Do we need to re-open the sound device?
  // Only do this when absolutely necessary
  if(myAudioSettings.sampleRate() != static_cast<uInt32>(myHardwareSpec.freq) ||
     myAudioSettings.fragmentSize() != static_cast<uInt32>(myHardwareSpec.samples) ||
     myAudioSettings.device() != myDeviceId)
    openDevice();

  myEmulationTiming = emulationTiming;
  myWavHandler.setSpeed(262 * 60 * 2. / myEmulationTiming->audioSampleRate());

  Logger::debug("SoundSDL::open started ...");

  audioQueue->ignoreOverflows(!myAudioSettings.enabled());
  if(!myAudioSettings.enabled())
  {
    Logger::info("Sound disabled\n");
    return;
  }

  myAudioQueue = audioQueue;
  myUnderrun = true;
  myCurrentFragment = nullptr;

  // Adjust volume to that defined in settings
  setVolume(myAudioSettings.volume());

  initResampler();

  // Show some info
  myAboutString = about();
  if(myAboutString != pre_about)
    Logger::info(myAboutString);

  // And start the SDL sound subsystem ...
  pause(false);

  Logger::debug("SoundSDL::open finished");
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void SoundSDL::mute(bool enable)
{
  if(enable)
    myVolumeFactor = 0;
  else
    setVolume(myAudioSettings.volume());
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void SoundSDL::toggleMute()
{
  const bool wasMuted = myVolumeFactor == 0;
  mute(!wasMuted);

  string message = "Sound ";
  message += !myAudioSettings.enabled()
    ? "disabled"
    : (wasMuted ? "unmuted" : "muted");

  myOSystem.frameBuffer().showTextMessage(message);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool SoundSDL::pause(bool enable)
{
  ASSERT_MAIN_THREAD;

  const bool wasPaused = SDL_GetAudioDeviceStatus(myDevice) == SDL_AUDIO_PAUSED;
  if(myIsInitializedFlag)
  {
    SDL_PauseAudioDevice(myDevice, enable ? 1 : 0);
    myWavHandler.pause(enable);
  }
  return wasPaused;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void SoundSDL::setVolume(uInt32 volume)
{
  if(myIsInitializedFlag && (volume <= 100))
  {
    myAudioSettings.setVolume(volume);
    myVolumeFactor = myAudioSettings.enabled() ? static_cast<float>(volume) / 100.F : 0;
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void SoundSDL::adjustVolume(int direction)
{
  Int32 percent = myAudioSettings.volume();
  percent = BSPF::clamp(percent + direction * 2, 0, 100);

  // Enable audio if it is currently disabled
  const bool enabled = myAudioSettings.enabled();

  if(percent > 0 && direction && !enabled)
  {
    setEnabled(true);
    myOSystem.console().initializeAudio();
  }
  setVolume(percent);

  // Now show an onscreen message
  ostringstream strval;
  (percent) ? strval << percent << "%" : strval << "Off";
  myOSystem.frameBuffer().showGaugeMessage("Volume", strval.view(), percent);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
string SoundSDL::about() const
{
  ostringstream buf;
  buf << "Sound enabled:\n"
      << "  Volume:   " << myAudioSettings.volume() << "%\n"
      << "  Device:   " << myDevices.at(myDeviceId).first << '\n'
      << "  Channels: " << static_cast<uInt32>(myHardwareSpec.channels)
      << (myAudioQueue->isStereo() ? " (Stereo)" : " (Mono)") << '\n'
      << "  Preset:   ";
  switch(myAudioSettings.preset())
  {
    using enum AudioSettings::Preset;
    case custom:
      buf << "Custom\n";
      break;
    case lowQualityMediumLag:
      buf << "Low quality, medium lag\n";
      break;
    case highQualityMediumLag:
      buf << "High quality, medium lag\n";
      break;
    case highQualityLowLag:
      buf << "High quality, low lag\n";
      break;
    case ultraQualityMinimalLag:
      buf << "Ultra quality, minimal lag\n";
      break;
    default:
      break;  // Not supposed to get here
  }
  buf << "    Fragment size: " << static_cast<uInt32>(myHardwareSpec.samples)
      << " bytes\n"
      << "    Sample rate:   " << static_cast<uInt32>(myHardwareSpec.freq)
      << " Hz\n";
  buf << "    Resampling:    ";
  switch(myAudioSettings.resamplingQuality())
  {
    using enum AudioSettings::ResamplingQuality;
    case nearestNeighbour:
      buf << "Quality 1, nearest neighbor\n";
      break;
    case lanczos_2:
      buf << "Quality 2, Lanczos (a = 2)\n";
      break;
    case lanczos_3:
      buf << "Quality 3, Lanczos (a = 3)\n";
      break;
    default:
      break;  // Not supposed to get here
  }
  buf << "    Headroom:      " << std::fixed << std::setprecision(1)
      << (0.5 * myAudioSettings.headroom()) << " frames\n"
      << "    Buffer size:   " << std::fixed << std::setprecision(1)
      << (0.5 * myAudioSettings.bufferSize()) << " frames\n";
  return buf.str();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void SoundSDL::initResampler()
{
  const Resampler::NextFragmentCallback nextFragmentCallback = [this] () -> Int16* {
    Int16* nextFragment = nullptr;

    if(myUnderrun)
      nextFragment = myAudioQueue->size() >= myEmulationTiming->prebufferFragmentCount()
        ? myAudioQueue->dequeue(myCurrentFragment)
        : nullptr;
    else
      nextFragment = myAudioQueue->dequeue(myCurrentFragment);

    myUnderrun = nextFragment == nullptr;
    if(nextFragment)
      myCurrentFragment = nextFragment;

    return nextFragment;
  };

  const Resampler::Format formatFrom =
    Resampler::Format(myEmulationTiming->audioSampleRate(),
    myAudioQueue->fragmentSize(), myAudioQueue->isStereo());
  const Resampler::Format formatTo =
    Resampler::Format(myHardwareSpec.freq, myHardwareSpec.samples,
    myHardwareSpec.channels > 1);

  switch(myAudioSettings.resamplingQuality())
  {
    using enum AudioSettings::ResamplingQuality;
    case nearestNeighbour:
      myResampler = make_unique<SimpleResampler>(formatFrom, formatTo,
                                                 nextFragmentCallback);
      break;

    case lanczos_2:
      myResampler = make_unique<LanczosResampler>(formatFrom, formatTo,
                                                  nextFragmentCallback, 2);
      break;

    case lanczos_3:
      myResampler = make_unique<LanczosResampler>(formatFrom, formatTo,
                                                  nextFragmentCallback, 3);
      break;

    default:
      throw runtime_error("invalid resampling quality");
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void SoundSDL::callback(void* object, uInt8* stream, int len)
{
  const auto* self = static_cast<SoundSDL*>(object);

  if(self->myAudioQueue && self->myResampler)
  {
    // The stream is 32-bit float (even though this callback is 8-bits), since
    // the resampler and TIA audio subsystem always generate float samples
    auto* s = reinterpret_cast<float*>(stream);
    const uInt32 length = len >> 2;
    self->myResampler->fillFragment(s, length);

    for(uInt32 i = 0; i < length; ++i)
      s[i] *= SoundSDL::myVolumeFactor;
  }
  else
    SDL_memset(stream, 0, len);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool SoundSDL::playWav(const string& fileName, uInt32 position, uInt32 length)
{
  const char* const device = myDeviceId
    ? myDevices.at(myDeviceId).first.c_str()
    : nullptr;

  return myWavHandler.play(fileName, device, position, length);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void SoundSDL::stopWav()
{
  myWavHandler.stop();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
uInt32 SoundSDL::wavSize() const
{
  return myWavHandler.size();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool SoundSDL::WavHandlerSDL::play(
    const string& fileName, const char* device, uInt32 position, uInt32 length)
{
  // Load WAV file
  if(fileName != myFilename || myBuffer == nullptr)
  {
    if(myBuffer)
    {
      SDL_FreeWAV(myBuffer);
      myBuffer = nullptr;
    }
    SDL_zero(mySpec);
    if(SDL_LoadWAV(fileName.c_str(), &mySpec, &myBuffer, &myLength) == nullptr)
      return false;

    // Set the callback function
    mySpec.callback = callback;
    mySpec.userdata = this;
  }
  if(position > myLength)
    return false;

  myFilename = fileName;

  myRemaining = length
    ? std::min(length, myLength - position)
    : myLength;
  myPos = myBuffer + position;

  // Open audio device
  if(!myDevice)
  {
    myDevice = SDL_OpenAudioDevice(device, 0, &mySpec, nullptr, 0);
    if(!myDevice)
      return false;

    // Play audio
    pause(false);
  }
  return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void SoundSDL::WavHandlerSDL::stop()
{
  if(myBuffer)
  {
    // Clean up
    myRemaining = 0;
    SDL_CloseAudioDevice(myDevice);  myDevice = 0;
    SDL_FreeWAV(myBuffer);  myBuffer = nullptr;
  }
  if(myCvtBuffer)
  {
    myCvtBuffer.reset();
    myCvtBufferSize = 0;
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void SoundSDL::WavHandlerSDL::processWav(uInt8* stream, uInt32 len)
{
  SDL_memset(stream, mySpec.silence, len);
  if(myRemaining)
  {
    if(mySpeed != 1.0)
    {
      const int origLen = len;
      len = std::round(len / mySpeed);
      const int newFreq =
        std::round(static_cast<double>(mySpec.freq) * origLen / len);

      if(len > myRemaining)  // NOLINT(readability-use-std-min-max)
        len = myRemaining;

      SDL_AudioCVT cvt;
      SDL_BuildAudioCVT(&cvt, mySpec.format, mySpec.channels, mySpec.freq,
                              mySpec.format, mySpec.channels, newFreq);
      SDL_assert(cvt.needed); // Obviously, this one is always needed.
      cvt.len = len * mySpec.channels;  // Mono 8 bit sample frames

      if(!myCvtBuffer || std::cmp_less(myCvtBufferSize, cvt.len * cvt.len_mult))
      {
        myCvtBufferSize = cvt.len * cvt.len_mult;
        myCvtBuffer = make_unique<uInt8[]>(myCvtBufferSize);
      }
      cvt.buf = myCvtBuffer.get();

      // Read original data into conversion buffer
      SDL_memcpy(cvt.buf, myPos, cvt.len);
      SDL_ConvertAudio(&cvt);
      // Mix volume adjusted WAV data into silent buffer
      SDL_MixAudioFormat(stream, cvt.buf, mySpec.format, cvt.len_cvt,
                         SDL_MIX_MAXVOLUME * SoundSDL::myVolumeFactor);
    }
    else
    {
      if(len > myRemaining)  // NOLINT(readability-use-std-min-max)
        len = myRemaining;

      // Mix volume adjusted WAV data into silent buffer
      SDL_MixAudioFormat(stream, myPos, mySpec.format, len,
                         SDL_MIX_MAXVOLUME * myVolumeFactor);
    }
    myPos += len;
    myRemaining -= len;
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void SoundSDL::WavHandlerSDL::callback(void* object, uInt8* stream, int len)
{
  static_cast<WavHandlerSDL*>(object)->processWav(
      stream, static_cast<uInt32>(len));
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
SoundSDL::WavHandlerSDL::~WavHandlerSDL()
{
  if(myDevice)
  {
    SDL_CloseAudioDevice(myDevice);
    SDL_FreeWAV(myBuffer);
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void SoundSDL::WavHandlerSDL::pause(bool state) const
{
  if(myDevice)
    SDL_PauseAudioDevice(myDevice, state ? 1 : 0);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
float SoundSDL::myVolumeFactor = 0.F;

#endif  // SOUND_SUPPORT
