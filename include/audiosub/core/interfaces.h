#pragma once

#include "audiosub/core/types.h"

namespace audiosub::core {

class ISubtitleConsumer {
 public:
  virtual ~ISubtitleConsumer() = default;
  virtual void OnSubtitleSegment(const SubtitleSegment& segment) = 0;
};

class IMarkChannel {
 public:
  virtual ~IMarkChannel() = default;
  virtual bool SendMark(const MarkMessage& mark) = 0;
};

class IASREngine {
 public:
  virtual ~IASREngine() = default;
  virtual void PushAudio(const PcmFrame& frame) = 0;
  virtual void SetSubtitleConsumer(ISubtitleConsumer* consumer) = 0;
};

class IFusionEngine {
 public:
  virtual ~IFusionEngine() = default;
  virtual EnhancedSubtitleSegment Fuse(const SubtitleSegment& subtitle) = 0;
  virtual void PushMark(const MarkMessage& mark) = 0;
};

}  // namespace audiosub::core
