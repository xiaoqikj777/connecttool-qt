#pragma once

#include <QByteArray>

class QWindow;
struct cs_audio_source_t;

class SoundNotifier {
public:
  SoundNotifier();
  ~SoundNotifier();

  bool initialize(QWindow *window);
  void playMessageAlert();
  bool isInitialized() const { return initialized_; }

private:
  bool loadAlertSample();

  bool initialized_ = false;
  QByteArray alertBuffer_;
  cs_audio_source_t *alertAudio_ = nullptr;
};
