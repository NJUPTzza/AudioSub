// main_window.h
// =============
// Qt 前端。两个类：
//   Backend    —— 持有 audiosub_capi.dll 的引擎句柄，注册 C 回调，把后台线程
//                 来的事件转成 Qt 信号（信号跨线程用 QueuedConnection 切回 UI）。
//   MainWindow —— 纯界面：状态栏、字幕/标注转写区、按住说话、标注输入、指标面板。
//
// 关键：C 回调发生在 DLL 内部后台线程，回调里把 const char* 立刻拷成 QString，
// 再 emit 信号；Qt 把信号参数拷贝后投递到主线程槽，UI 操作全在主线程。

#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <QMainWindow>
#include <QString>

#include "audiosub_capi.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;
class QScrollArea;
class QVBoxLayout;
class QWidget;

// ---- Backend：引擎句柄 + C 回调 -> Qt 信号 ----
class Backend : public QObject {
  Q_OBJECT
 public:
  explicit Backend(QObject* parent = nullptr);
  ~Backend() override;

  // 启动引擎。返回是否成功。
  bool start(const QString& id, const QString& host, int port,
             const QString& audioPath);
  bool isOfferer() const;
  void setTalking(bool on);
  void sendNote(const QString& text);
  void stop();
  AudiosubMetrics metrics() const;

 signals:
  // 这些信号都从后台线程 emit，连接到 MainWindow 槽时用 QueuedConnection。
  // 所有参数都是值类型（QString/数值/QStringList），可安全跨线程拷贝投递。
  void stateEvent(QString line);
  void subtitleEvent(QString text, qint64 startMs, qint64 endMs,
                     qint64 latencyMs, bool remote, QStringList markLines);
  void markEvent(QString text, qint64 visMs);
  void orphanEvent(QString text);

 private:
  // C 回调蹦床（trampoline）：user 即 this。
  static void OnState(void* user, const char* s);
  static void OnSubtitle(void* user, const AudiosubSubtitle* sub);
  static void OnMark(void* user, uint64_t seq, const char* text, int64_t vis);
  static void OnOrphan(void* user, uint64_t seq, const char* text);

  AudiosubEngineHandle* h_ = nullptr;

  // 专属引擎线程：WebRTC 的 COM 初始化必须在这个（非 Qt 主）线程上完成并常驻，
  // 避免和 Qt 主线程的 STA 套间冲突。线程在 start() 里拉起、stop() 里 join。
  std::thread engine_thread_;
  std::mutex stop_mutex_;
  std::condition_variable stop_cv_;
  bool stop_requested_ = false;
  std::atomic<bool> thread_running_{false};
};

// ---- MainWindow：界面 ----
class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  MainWindow(const QString& id, const QString& host, int port,
             const QString& audioPath, QWidget* parent = nullptr);
  ~MainWindow() override;

 protected:
  void closeEvent(QCloseEvent* e) override;

 private slots:
  void onState(QString line);
  void onSubtitle(QString text, qint64 startMs, qint64 endMs, qint64 latencyMs,
                  bool remote, QStringList markLines);
  void onMark(QString text, qint64 visMs);
  void onOrphan(QString text);
  void onTalkToggled(bool on);
  void onSendNote();
  void refreshMetrics();

 private:
  // 往聊天区追加一条气泡消息（mine=true 靠右蓝色，否则靠左白色）。
  // body 为已构造好的富文本，footer 为底部浅灰小字（可空）。
  void appendMessage(bool mine, const QString& name, const QString& bodyHtml,
                     const QString& footer);
  // 居中的系统提示（连接状态等）。
  void appendSystem(const QString& text);
  // 滚动到底部。
  void scrollToBottom();

  Backend* backend_ = nullptr;
  bool isOfferer_ = false;
  QString ownName_;   // 本端显示名
  QString peerName_;  // 对端显示名

  QLabel* statusLabel_ = nullptr;     // 侧栏联系人下方状态行
  QLabel* headerTitle_ = nullptr;     // 右侧顶栏标题（对端名）
  QLabel* contactName_ = nullptr;     // 侧栏联系人名
  QLabel* contactAvatar_ = nullptr;   // 侧栏联系人头像
  QScrollArea* chatScroll_ = nullptr;
  QWidget* chatBody_ = nullptr;
  QVBoxLayout* messagesLayout_ = nullptr;
  QPushButton* talkButton_ = nullptr;
  QLineEdit* noteEdit_ = nullptr;
  QPushButton* noteButton_ = nullptr;
  QLabel* metricLat_ = nullptr;
  QLabel* metricErr_ = nullptr;
  QLabel* metricVis_ = nullptr;
  QTimer* metricTimer_ = nullptr;
};
