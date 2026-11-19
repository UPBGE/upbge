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

/* Play Sound's tickets (2026-11-17): pending until their flush, then the sound's own state
 * (Playing, Paused, Ended), Ended for a sound that never started. */
TEST(LN_SoundGroups, ATicketFollowsItsSound)
{
  ScopedReadDevice audio;
  LN_CommandBuffer::CacheToneForTests("LN Test Short", 440.0f, 0.05f);
  LN_CommandBuffer::CacheToneForTests("LN Test Long", 330.0f);
  EXPECT_EQ(LN_GetSoundTicketStatus(0), LN_SoundTicketStatus::Ended) << "no ticket: no sound";
  const uint64_t short_ticket = LN_NewSoundTicket();
  const uint64_t long_ticket = LN_NewSoundTicket();
  const uint64_t missing_ticket = LN_NewSoundTicket();
  EXPECT_EQ(LN_GetSoundTicketStatus(short_ticket), LN_SoundTicketStatus::Pending);
  FlushOne([&](LN_CommandBuffer &buffer) {
    buffer.AppendPlaySound("LN Test Short", 1.0f, 1.0f, false, 10, 1, 0, nullptr, short_ticket);
    buffer.AppendPlaySound("LN Test Long", 0.5f, 1.0f, true, 11, 1, 0, nullptr, long_ticket);
    buffer.AppendPlaySound("LN Test Nothing", 1.0f, 1.0f, false, 12, 1, 0, nullptr, missing_ticket);
  });
  EXPECT_EQ(LN_GetSoundTicketStatus(short_ticket), LN_SoundTicketStatus::Playing);
  EXPECT_EQ(LN_GetSoundTicketStatus(long_ticket), LN_SoundTicketStatus::Playing);
  EXPECT_EQ(LN_GetSoundTicketStatus(missing_ticket), LN_SoundTicketStatus::Ended)
      << "no such sound: it ended at once";
  audio.Loudness(); /* 0.2 s: the short tone (0.05 s) ends */
  EXPECT_EQ(LN_GetSoundTicketStatus(short_ticket), LN_SoundTicketStatus::Ended);
  EXPECT_EQ(LN_GetSoundTicketStatus(long_ticket), LN_SoundTicketStatus::Playing);
  FlushOne([](LN_CommandBuffer &buffer) { buffer.AppendPauseSound("LN Test Long", 10, 1); });
  EXPECT_EQ(LN_GetSoundTicketStatus(long_ticket), LN_SoundTicketStatus::Paused);
  FlushOne([](LN_CommandBuffer &buffer) { buffer.AppendStopSound("LN Test Long", 10, 1); });
  EXPECT_EQ(LN_GetSoundTicketStatus(long_ticket), LN_SoundTicketStatus::Ended);
}

/* Play Sound 3D's At Position (2026-11-17): no emitter, so two plays of one sound from one tree
 * sound together, where at the speaker the second restarts the first. */
TEST(LN_SoundGroups, SoundsAtPositionsPlayBesideEachOther)
{
  ScopedReadDevice audio;
  LN_CommandBuffer::CacheToneForTests("LN Test Boom", 220.0f);
  /* (No speaker object: the rig has none; the restart rule keys on the speaker, here none.) */
  KX_GameObject *const owner = nullptr;
  FlushOne([&](LN_CommandBuffer &buffer) {
    for (int k = 0; k < 2; k++) {
      buffer.AppendPlaySound3D(owner, "LN Test Boom", 0.25f, 1.0f, true, 10, 1, 2, 0, 0, 0, true,
                               MT_Vector3(float(k), 0.0f, 0.0f));
    }
  });
  const float both = audio.Loudness();
  FlushOne([](LN_CommandBuffer &buffer) { buffer.AppendStopSound("LN Test Boom", 10, 1); });
  EXPECT_LT(audio.Loudness(), 0.001f);
  FlushOne([&](LN_CommandBuffer &buffer) {
    for (int k = 0; k < 2; k++) {
      buffer.AppendPlaySound3D(owner, "LN Test Boom", 0.25f, 1.0f, true, 10, 1, 2, 0, 0);
    }
  });
  const float restarted = audio.Loudness();
  EXPECT_GT(restarted, 0.01f);
  EXPECT_NEAR(both, 2.0f * restarted, 0.02f) << "two copies at positions, one at the speaker";
}

#endif /* WITH_AUDASPACE */

}  // namespace

#ifdef WITH_AUDASPACE

#  include "LN_SemRig.h"

using namespace ln_test;
using namespace ln_test::sem;

/* The nodes (2026-11-17): a Sound linked from a value (here a property holding its name), Playing from the step
 * after it starts, Finished once when it ends; in every mode. */
TEST(LN_SoundNodes, PlayingAndFinishedFollowTheSoundStartedLast)
{
  for (const bool spatial : {false, true}) {
    SCOPED_TRACE(spatial ? "Play Sound 3D" : "Play Sound");
    TreeBuilder b;
    GenNode &step = b.Add("LogicNativeOnFixedUpdate");
    GenNode &start = b.Add("LogicNativeGetGamePropertyBool");
    b.String(start, "Property", "play");
    GenNode &edge = b.Add("LogicNativeBooleanEdge");
    b.Link(start, "Value", edge, "Condition");
    /* (A value, as a list's item gives it: Get Random List Item of sounds.) */
    GenNode &name = b.Add("LogicNativeGetGameProperty");
    b.String(name, "Property", "sound");
    GenNode &play = b.Add(spatial ? "LogicNativePlaySound3D" : "LogicNativePlaySound");
    b.Link(edge, "Rising", play, "Flow");
    b.Link(name, "Value", play, "Sound");
    GenNode &playing = b.Add("LogicNativeSetGamePropertyBool");
    b.String(playing, "Property", "playing");
    b.Link(play, "Playing", playing, "Value");
    b.Link(step, "Out", playing, "Flow");
    GenNode &branch = b.Add("LogicNativeBranch");
    b.Link(play, "Finished", branch, "Condition");
    b.Link(playing, "Done", branch, "Flow");
    GenNode &count = b.Add("LogicNativeModifyProperty", int16_t(blender::NODE_MATH_ADD));
    b.String(count, "Property", "finished");
    b.Float(count, "Value", 1.0f);
    b.Link(branch, "True", count, "Flow");
    std::string report;
    const std::shared_ptr<LN_Program> program = b.Compile(&report);
    ASSERT_NE(program, nullptr) << report;
    std::string reference;
    for (const SemMode &mode : AllModes()) {
      SCOPED_TRACE(mode.name);
      ScopedReadDevice audio;
      LN_CommandBuffer::CacheToneForTests("LN Test Short", 440.0f, 0.05f);
      SemRig rig(program, mode, 1);
      KX_GameObject &object = rig.Object(0);
      rig.SetString(object, "sound", "LN Test Short");
      rig.SetFloat(object, "finished", 0.0f);
      std::string text;
      for (int s = 0; s < 8; s++) {
        rig.SetBool(object, "play", s >= 2);
        rig.Step();
        if (s == 4) {
          audio.Loudness(); /* 0.2 s of sound: the tone (0.05 s) ends */
        }
        text += std::to_string(int(rig.Float(object, "playing"))) +
                std::to_string(int(rig.Float(object, "finished"))) + " ";
      }
      /* Started in step 2 (its flush), playing from step 3 until the audio moved on after step
       * 4, finished in step 5 (seen once). */
      EXPECT_EQ(text, "00 00 00 10 10 01 01 01 ");
      if (reference.empty()) {
        reference = text;
      }
      EXPECT_EQ(text, reference);
    }
  }
}

#endif /* WITH_AUDASPACE */
