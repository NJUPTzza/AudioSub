// main.cpp（Qt 前端入口）
// 解析命令行 --id/--host/--port/--audio-path，开窗口。
// 注意：whisper 模型路径在引擎内部是相对工作目录的
// "third_party/whisper.cpp/models/ggml-small.bin"，所以请从仓库根目录启动本程序。

#include <QApplication>
#include <QString>

#include "main_window.h"

int main(int argc, char** argv) {
  QApplication app(argc, argv);

  QString id;
  QString host = "127.0.0.1";
  int port = 8888;
  QString audioPath = "webrtc";  // GUI 默认走 WebRTC 音轨链路

  const QStringList args = app.arguments();
  for (int i = 1; i < args.size(); ++i) {
    const QString& a = args[i];
    auto next = [&]() -> QString {
      return (i + 1 < args.size()) ? args[++i] : QString();
    };
    if (a == "--id") id = next();
    else if (a == "--host") host = next();
    else if (a == "--port") port = next().toInt();
    else if (a == "--audio-path") audioPath = next();
  }

  if (id != "A" && id != "B") id = "A";  // 缺省给个 A，避免空跑

  MainWindow w(id, host, port, audioPath);
  w.show();
  return app.exec();
}
