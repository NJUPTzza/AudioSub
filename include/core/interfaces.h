#pragma once

#include <memory>

#include "core/types.h"

namespace audiosub::core {

// Receives decoded PCM frames from WebRTC sinks, replay tools, or tests.
class IAudioFrameConsumer {
 public:
  virtual ~IAudioFrameConsumer() = default;
  virtual void OnPcmFrame(const PcmFrame& frame) = 0;
};

// Receives subtitle output from an ASR engine or subtitle pipeline.
class ISubtitleConsumer {
 public:
  virtual ~ISubtitleConsumer() = default;
  virtual void OnSubtitleSegment(const SubtitleSegment& segment) = 0;
};

// Sends/receives structured annotation messages.
class IMarkChannel {
 public:
  virtual ~IMarkChannel() = default;
  virtual bool SendMark(const MarkMessage& mark) = 0;
};

// Converts audio into subtitle segments. Implementations may run incrementally.
class IASREngine {
 public:
  virtual ~IASREngine() = default;
  virtual void PushAudio(const PcmFrame& frame) = 0;
  virtual void SetSubtitleConsumer(ISubtitleConsumer* consumer) = 0;
};

// Aligns subtitle and annotation streams into a single user-facing result.
class IFusionEngine {
 public:
  virtual ~IFusionEngine() = default;
  virtual EnhancedSubtitleSegment Fuse(const SubtitleSegment& subtitle) = 0;
  virtual void PushMark(const MarkMessage& mark) = 0;
};

using AudioFrameConsumerPtr = std::shared_ptr<IAudioFrameConsumer>;
using SubtitleConsumerPtr = std::shared_ptr<ISubtitleConsumer>;
using MarkChannelPtr = std::shared_ptr<IMarkChannel>;
using ASREnginePtr = std::shared_ptr<IASREngine>;
using FusionEnginePtr = std::shared_ptr<IFusionEngine>;

}  // namespace audiosub::core
