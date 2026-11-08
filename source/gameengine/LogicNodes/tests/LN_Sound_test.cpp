/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

#include <cstring>

#include "DNA_sound_types.h"
#include "DNA_speaker_types.h"

#include "LN_CommandBuffer.h"
#include "LN_SoundGroups.h"

#ifdef WITH_AUDASPACE
#  include <devices/DeviceManager.h>
#  include <devices/ReadDevice.h>
#endif

namespace {

/* Play Sound 3D at a Blender Speaker object plays as Blender plays speakers (2026-11-08, the
 * removed Start Speaker's job): its own sound when the node picks none, the node's volume and
 * pitch times the speaker's, nothing while it is muted. Any other object: the node's own. */
TEST(LN_SoundGroups, APlaySound3DAtASpeakerPlaysItsSoundWithItsSettings)
{
  using Play = LN_CommandBuffer::SpeakerPlay;
  blender::bSound sound;
  std::strcpy(sound.id.name, "SOStep");
  blender::Speaker speaker;
  speaker.sound = &sound;
  speaker.volume = 0.5f;
  speaker.pitch = 2.0f;

  Play play = LN_CommandBuffer::SpeakerPlayOf(&speaker, "", 0.8f, 1.5f);
  EXPECT_EQ(play.sound, "Step");
  EXPECT_FLOAT_EQ(play.volume, 0.4f);
  EXPECT_FLOAT_EQ(play.pitch, 3.0f);
  EXPECT_FALSE(play.muted);

  play = LN_CommandBuffer::SpeakerPlayOf(&speaker, "Bell", 1.0f, 1.0f);
  EXPECT_EQ(play.sound, "Bell") << "a picked Sound wins over the speaker's own";
  EXPECT_FLOAT_EQ(play.volume, 0.5f);

  speaker.flag = blender::eSpeaker_Flag(speaker.flag | blender::SPK_MUTED);
  EXPECT_TRUE(LN_CommandBuffer::SpeakerPlayOf(&speaker, "", 1.0f, 1.0f).muted);

  speaker.sound = nullptr;
  EXPECT_EQ(LN_CommandBuffer::SpeakerPlayOf(&speaker, "", 1.0f, 1.0f).sound, "")
      << "no Sound and a speaker without one: nothing to play";

  play = LN_CommandBuffer::SpeakerPlayOf(nullptr, "Bell", 0.7f, 1.25f);
  EXPECT_EQ(play.sound, "Bell");
  EXPECT_FLOAT_EQ(play.volume, 0.7f);
  EXPECT_FLOAT_EQ(play.pitch, 1.25f);
  EXPECT_FALSE(play.muted);
}

TEST(LN_SoundGroups, GroupsStartFullAndChangeAtOnceOrOverAFade)
{
  LN_SoundGroupVolumes groups;
  for (int32_t group = 0; group < LN_SOUND_GROUP_COUNT; ++group) {
    EXPECT_EQ(groups.Volume(LN_SoundGroup(group)), 1.0f);
  }
  /* At once. */
  groups.Set(LN_SoundGroup::Music, 0.25f, 0.0f, 10.0);
  EXPECT_FLOAT_EQ(groups.Volume(LN_SoundGroup::Music), 0.25f);
  EXPECT_FALSE(groups.Fading());

  /* Over 2 seconds: halfway after one. */
  groups.Set(LN_SoundGroup::Effects, 0.0f, 2.0f, 10.0);
  EXPECT_TRUE(groups.Fading());
  EXPECT_FLOAT_EQ(groups.Volume(LN_SoundGroup::Effects), 1.0f);
  EXPECT_TRUE(groups.Advance(11.0));
  EXPECT_FLOAT_EQ(groups.Volume(LN_SoundGroup::Effects), 0.5f);
  /* A new change halfway through starts where the volume is. */
  groups.Set(LN_SoundGroup::Effects, 1.0f, 1.0f, 11.0);
  EXPECT_TRUE(groups.Advance(11.5));
  EXPECT_FLOAT_EQ(groups.Volume(LN_SoundGroup::Effects), 0.75f);
  EXPECT_TRUE(groups.Advance(20.0));
  EXPECT_FLOAT_EQ(groups.Volume(LN_SoundGroup::Effects), 1.0f);
  EXPECT_FALSE(groups.Fading());
  EXPECT_FALSE(groups.Advance(21.0));
  /* Other groups keep theirs. */
  EXPECT_FLOAT_EQ(groups.Volume(LN_SoundGroup::Music), 0.25f);
  EXPECT_FLOAT_EQ(groups.Volume(LN_SoundGroup::Master), 1.0f);

  /* A negative or invalid volume is silence, never an error. */
  groups.Set(LN_SoundGroup::UI, -3.0f, 0.0f, 0.0);
  EXPECT_EQ(groups.Volume(LN_SoundGroup::UI), 0.0f);
  groups.Set(LN_SoundGroup::UI, std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0);
  EXPECT_EQ(groups.Volume(LN_SoundGroup::UI), 0.0f);

  groups.Reset();
  for (int32_t group = 0; group < LN_SOUND_GROUP_COUNT; ++group) {
    EXPECT_EQ(groups.Volume(LN_SoundGroup(group)), 1.0f);
  }

  /* Node settings: a sound is never in Master; unknown numbers are Effects. */
  EXPECT_EQ(LN_SoundGroupFromInt(5, true), LN_SoundGroup::Master);
  EXPECT_EQ(LN_SoundGroupFromInt(5, false), LN_SoundGroup::Effects);
  EXPECT_EQ(LN_SoundGroupFromInt(1, false), LN_SoundGroup::Music);
  EXPECT_EQ(LN_SoundGroupFromInt(-1, true), LN_SoundGroup::Effects);
  EXPECT_EQ(LN_SoundGroupFromInt(42, true), LN_SoundGroup::Effects);
}

#ifdef WITH_AUDASPACE

/** The game's audio device for one test: a device the test reads the mix from. */
class ScopedReadDevice {
 public:
  ScopedReadDevice()
  {
    aud::DeviceSpecs specs;
    specs.format = aud::FORMAT_FLOAT32;
    specs.rate = aud::RATE_48000;
    specs.channels = aud::CHANNELS_MONO;
    device = std::make_shared<aud::ReadDevice>(specs);
    m_previous = aud::DeviceManager::getDevice();
    aud::DeviceManager::setDevice(device);
    LN_CommandBuffer::ResetSoundGroups();
  }
  ~ScopedReadDevice()
  {
    LN_CommandBuffer command_buffer;
    command_buffer.BeginRecording();
    command_buffer.AppendStopAllSounds(1, 1);
    command_buffer.EndRecording();
    command_buffer.Flush();
    LN_CommandBuffer::ResetSoundGroups();
    if (m_previous) {
      aud::DeviceManager::setDevice(m_previous);
    }
    else {
      aud::DeviceManager::releaseDevice();
    }
  }

  /** How loud the mix is now: the root mean square of a tenth of a second, read after another
   * tenth (a volume change fades in over one read). */
  float Loudness()
  {
    std::vector<float> samples(4800);
    device->read(reinterpret_cast<aud::data_t *>(samples.data()), int(samples.size()));
    device->read(reinterpret_cast<aud::data_t *>(samples.data()), int(samples.size()));
    double sum = 0.0;
    for (const float sample : samples) {
      sum += double(sample) * double(sample);
    }
    return float(std::sqrt(sum / double(samples.size())));
  }

  std::shared_ptr<aud::ReadDevice> device;

 private:
  std::shared_ptr<aud::IDevice> m_previous;
};

void FlushOne(const std::function<void(LN_CommandBuffer &)> &record)
{
  LN_CommandBuffer command_buffer;
  command_buffer.BeginRecording();
  record(command_buffer);
  command_buffer.EndRecording();
  command_buffer.Flush();
}

/* A full-scale sine is 0.707 loud (root mean square). */
constexpr float kSineLoudness = 0.70710678f;

TEST(LN_SoundGroups, SetVolumeChangesThePlayingSoundsOfItsGroup)
{
  ScopedReadDevice audio;
  LN_CommandBuffer::CacheToneForTests("LN Test Music", 440.0f);
  EXPECT_LT(audio.Loudness(), 0.001f);

  /* Play Sound in the Music group at its own volume 0.8. */
  FlushOne([](LN_CommandBuffer &buffer) {
    buffer.AppendPlaySound(
        "LN Test Music", 0.8f, 1.0f, true, 10, 1, int32_t(LN_SoundGroup::Music));
  });
  EXPECT_NEAR(audio.Loudness(), 0.8f * kSineLoudness, 0.01f);

  /* The Music group at half: the music already playing follows. */
  FlushOne([](LN_CommandBuffer &buffer) {
    buffer.AppendSetSoundVolume(int32_t(LN_SoundGroup::Music), 0.5f, 0.0f, 10, 1);
  });
  EXPECT_FLOAT_EQ(LN_CommandBuffer::SoundGroupVolume(int32_t(LN_SoundGroup::Music)), 0.5f);
  EXPECT_NEAR(audio.Loudness(), 0.4f * kSineLoudness, 0.01f);

  /* Another group changes nothing here. */
  FlushOne([](LN_CommandBuffer &buffer) {
    buffer.AppendSetSoundVolume(int32_t(LN_SoundGroup::Effects), 0.0f, 0.0f, 10, 1);
  });
  EXPECT_NEAR(audio.Loudness(), 0.4f * kSineLoudness, 0.01f);

  /* Master scales every sound (the device). */
  FlushOne([](LN_CommandBuffer &buffer) {
    buffer.AppendSetSoundVolume(int32_t(LN_SoundGroup::Master), 0.5f, 0.0f, 10, 1);
  });
  EXPECT_NEAR(audio.Loudness(), 0.2f * kSineLoudness, 0.01f);
  EXPECT_FLOAT_EQ(audio.device->getVolume(), 0.5f);

  /* A fade: nothing yet, silence once it is over (the frame update moves it on). */
  FlushOne([](LN_CommandBuffer &buffer) {
    buffer.AppendSetSoundVolume(int32_t(LN_SoundGroup::Music), 0.0f, 0.2f, 10, 1);
  });
  LN_CommandBuffer::UpdateSoundGroups();
  EXPECT_GT(audio.Loudness(), 0.1f * kSineLoudness);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  LN_CommandBuffer::UpdateSoundGroups();
  EXPECT_FLOAT_EQ(LN_CommandBuffer::SoundGroupVolume(int32_t(LN_SoundGroup::Music)), 0.0f);
  EXPECT_LT(audio.Loudness(), 0.001f);

  /* A new game: full volume again, and the device's own volume back. */
  LN_CommandBuffer::ResetSoundGroups();
  EXPECT_FLOAT_EQ(LN_CommandBuffer::SoundGroupVolume(int32_t(LN_SoundGroup::Music)), 1.0f);
  EXPECT_FLOAT_EQ(audio.device->getVolume(), 1.0f);
}

TEST(LN_SoundGroups, AMenuSoundInTheUIGroupFollowsSetVolumeUI)
{
  ScopedReadDevice audio;
  LN_CommandBuffer::CacheToneForTests("LN Test Click", 880.0f);
  /* On UI Sound -> Play Sound, group UI, at full volume. */
  FlushOne([](LN_CommandBuffer &buffer) {
    buffer.AppendPlaySound("LN Test Click", 1.0f, 1.0f, true, 10, 1, int32_t(LN_SoundGroup::UI));
  });
  EXPECT_NEAR(audio.Loudness(), kSineLoudness, 0.01f);

  /* Set Volume UI turns them down, also one that plays. */
  FlushOne([](LN_CommandBuffer &buffer) {
    buffer.AppendSetSoundVolume(int32_t(LN_SoundGroup::UI), 0.25f, 0.0f, 10, 1);
  });
  EXPECT_NEAR(audio.Loudness(), 0.25f * kSineLoudness, 0.01f);
  /* Music does not touch them. */
  FlushOne([](LN_CommandBuffer &buffer) {
    buffer.AppendSetSoundVolume(int32_t(LN_SoundGroup::Music), 0.0f, 0.0f, 10, 1);
  });
  EXPECT_NEAR(audio.Loudness(), 0.25f * kSineLoudness, 0.01f);
}

TEST(LN_SoundGroups, StopSoundStillFindsAGroupedSoundByName)
{
  ScopedReadDevice audio;
  LN_CommandBuffer::CacheToneForTests("LN Test Wind", 220.0f);
  FlushOne([](LN_CommandBuffer &buffer) {
    buffer.AppendPlaySound(
        "LN Test Wind", 1.0f, 1.0f, true, 10, 1, int32_t(LN_SoundGroup::Ambience));
  });
  EXPECT_GT(audio.Loudness(), 0.5f);
  FlushOne([](LN_CommandBuffer &buffer) { buffer.AppendStopSound("LN Test Wind", 10, 1); });
  EXPECT_LT(audio.Loudness(), 0.001f);
}

/* A sound plays once at a time per emitter (2026-11-08; per name before, so two enemies' footsteps
 * cut each other off): the same sound from two objects plays twice, from one object twice it
 * restarts (one copy), and Stop Sound stops every copy of the name. Two copies started in one
 * flush are in phase: twice as loud as one. (Emitters are only compared: fake pointers.) */
TEST(LN_SoundGroups, EachEmitterPlaysItsOwnCopyOfASound)
{
  ScopedReadDevice audio;
  LN_CommandBuffer::CacheToneForTests("LN Test Step", 330.0f);
  KX_GameObject *const enemy_a = reinterpret_cast<KX_GameObject *>(uintptr_t(0x1000));
  KX_GameObject *const enemy_b = reinterpret_cast<KX_GameObject *>(uintptr_t(0x2000));
  FlushOne([&](LN_CommandBuffer &buffer) {
    buffer.AppendPlaySound("LN Test Step", 0.25f, 1.0f, true, 10, 1, 0, enemy_a);
    buffer.AppendPlaySound("LN Test Step", 0.25f, 1.0f, true, 11, 1, 0, enemy_b);
  });
  EXPECT_NEAR(audio.Loudness(), 0.5f * kSineLoudness, 0.01f);
  FlushOne([](LN_CommandBuffer &buffer) { buffer.AppendStopSound("LN Test Step", 10, 1); });
  EXPECT_LT(audio.Loudness(), 0.001f);

  /* One emitter twice in a flush: the second play restarts it, one copy. */
  FlushOne([&](LN_CommandBuffer &buffer) {
    buffer.AppendPlaySound("LN Test Step", 0.25f, 1.0f, true, 10, 1, 0, enemy_a);
    buffer.AppendPlaySound("LN Test Step", 0.25f, 1.0f, true, 10, 2, 0, enemy_a);
  });
  EXPECT_NEAR(audio.Loudness(), 0.25f * kSineLoudness, 0.01f);
}

/* At the cap (256 playing sounds) the oldest sound that does not loop stops, not the looping
 * music started first (a crowd's footsteps must not end the background music). */
TEST(LN_SoundGroups, TheVoiceCapStopsTheOldestSoundThatDoesNotLoop)
{
  ScopedReadDevice audio;
  LN_CommandBuffer::CacheToneForTests("LN Test Music", 440.0f);
  LN_CommandBuffer::CacheToneForTests("LN Test Step", 330.0f);
  FlushOne([](LN_CommandBuffer &buffer) {
    buffer.AppendPlaySound(
        "LN Test Music", 0.5f, 1.0f, true, 10, 1, int32_t(LN_SoundGroup::Music));
  });
  /* 300 silent steps that do not loop, each from its own object (the tone never ends). */
  FlushOne([](LN_CommandBuffer &buffer) {
    for (uintptr_t k = 0; k < 300; k++) {
      buffer.AppendPlaySound("LN Test Step",
                             0.0f,
                             1.0f,
                             false,
                             10,
                             1,
                             0,
                             reinterpret_cast<KX_GameObject *>(uintptr_t(0x1000) + k * 16));
    }
  });
  FlushOne([](LN_CommandBuffer &buffer) { buffer.AppendStopSound("LN Test Step", 10, 1); });
  EXPECT_NEAR(audio.Loudness(), 0.5f * kSineLoudness, 0.01f);
}

#endif /* WITH_AUDASPACE */

}  // namespace
