// main_window.cpp
#include "main_window.h"

#include <future>

#include <QCloseEvent>
#include <QDateTime>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

// ============================ Backend ============================

Backend::Backend(QObject* parent) : QObject(parent) {
  h_ = audiosub_create();
  // 注册回调，user 传 this。回调里转 QString 后 emit。
  audiosub_set_state_cb(h_, &Backend::OnState, this);
  audiosub_set_subtitle_cb(h_, &Backend::OnSubtitle, this);
  audiosub_set_mark_cb(h_, &Backend::OnMark, this);
  audiosub_set_orphan_cb(h_, &Backend::OnOrphan, this);
}

Backend::~Backend() {
  stop();
  if (h_) {
    audiosub_destroy(h_);
    h_ = nullptr;
  }
}

bool Backend::start(const QString& id, const QString& host, int port,
                    const QString& audioPath) {
  // 在专属线程上启动引擎：WebRTC 的 COM(MTA) 初始化要远离 Qt 主线程的 STA。
  // 线程启动后会一直常驻（阻塞在 stop_cv_ 上），保证 COM 在其生命周期内有效；
  // start() 通过 promise 同步拿到启动结果再返回。
  std::promise<bool> ready;
  std::future<bool> fut = ready.get_future();

  const std::string id_s = id.toUtf8().constData();
  const std::string host_s = host.toUtf8().constData();
  const std::string audio_s = audioPath.toUtf8().constData();

  thread_running_ = true;
  engine_thread_ = std::thread([this, id_s, host_s, port, audio_s, &ready]() {
    const bool ok = audiosub_start(h_, id_s.c_str(), host_s.c_str(), port,
                                   audio_s.c_str()) != 0;
    ready.set_value(ok);
    // 常驻，直到 stop() 请求退出。
    std::unique_lock<std::mutex> lk(stop_mutex_);
    stop_cv_.wait(lk, [this] { return stop_requested_; });
    audiosub_stop(h_);
  });

  return fut.get();
}

bool Backend::isOfferer() const { return audiosub_is_offerer(h_) != 0; }

void Backend::setTalking(bool on) { audiosub_set_talking(h_, on ? 1 : 0); }

void Backend::sendNote(const QString& text) {
  audiosub_send_note(h_, text.toUtf8().constData());
}

void Backend::stop() {
  if (!thread_running_.exchange(false)) return;  // 只停一次
  {
    std::lock_guard<std::mutex> lk(stop_mutex_);
    stop_requested_ = true;
  }
  stop_cv_.notify_all();
  if (engine_thread_.joinable()) engine_thread_.join();  // 线程内会调 audiosub_stop
}

AudiosubMetrics Backend::metrics() const {
  AudiosubMetrics m{};
  audiosub_get_metrics(h_, &m);
  return m;
}

void Backend::OnState(void* user, const char* s) {
  auto* self = static_cast<Backend*>(user);
  emit self->stateEvent(QString::fromUtf8(s));
}

void Backend::OnSubtitle(void* user, const AudiosubSubtitle* sub) {
  auto* self = static_cast<Backend*>(user);
  // 把每条对齐到的标注预先格式化成一行，随字幕一起带出去。
  QStringList marks;
  for (int i = 0; i < sub->mark_count; ++i) {
    const AudiosubMark& mk = sub->marks[i];
    QString line = QString("📌 %1  · 误差%2ms")
                       .arg(QString::fromUtf8(mk.text))
                       .arg(mk.err_ms);
    if (mk.err_ms > 500) line += " ⚠";
    marks << line;
  }
  emit self->subtitleEvent(QString::fromUtf8(sub->text), sub->start_ms,
                           sub->end_ms, sub->latency_ms, sub->remote != 0,
                           marks);
}

void Backend::OnMark(void* user, uint64_t seq, const char* text, int64_t vis) {
  auto* self = static_cast<Backend*>(user);
  (void)seq;
  emit self->markEvent(QString::fromUtf8(text), vis);
}

void Backend::OnOrphan(void* user, uint64_t seq, const char* text) {
  auto* self = static_cast<Backend*>(user);
  (void)seq;
  emit self->orphanEvent(QString::fromUtf8(text));
}

// ============================ MainWindow ============================

// 纯灰色圆形头像，中间放名字首字母。
static QLabel* makeAvatar(const QString& name, QWidget* parent) {
  auto* a = new QLabel(parent);
  a->setObjectName("avatar");
  a->setFixedSize(36, 36);
  a->setAlignment(Qt::AlignCenter);
  a->setText(name.left(1).toUpper());
  return a;
}

MainWindow::MainWindow(const QString& id, const QString& host, int port,
                       const QString& audioPath, QWidget* parent)
    : QMainWindow(parent) {
  setWindowTitle(QString("AudioSub - %1").arg(id));
  resize(900, 640);

  // 全局字体（飞书常用无衬线中文字体）。
  setFont(QFont("Microsoft YaHei UI", 10));

  ownName_ = id;
  peerName_ = (id.compare("A", Qt::CaseInsensitive) == 0) ? "B" : "A";

  auto* central = new QWidget(this);
  central->setObjectName("central");
  auto* root = new QHBoxLayout(central);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  // ===== 左侧会话列表（1 对 1，仅一个对端会话）=====
  auto* sidebar = new QFrame(central);
  sidebar->setObjectName("sidebar");
  sidebar->setFixedWidth(220);
  auto* sv = new QVBoxLayout(sidebar);
  sv->setContentsMargins(0, 0, 0, 0);
  sv->setSpacing(0);

  auto* sideTitle = new QLabel("消息", sidebar);
  sideTitle->setObjectName("sideTitle");
  sv->addWidget(sideTitle);

  auto* contact = new QFrame(sidebar);
  contact->setObjectName("contact");
  auto* cl = new QHBoxLayout(contact);
  cl->setContentsMargins(14, 12, 14, 12);
  cl->setSpacing(10);
  contactAvatar_ = makeAvatar(peerName_, contact);
  auto* cc = new QVBoxLayout();
  cc->setSpacing(3);
  contactName_ = new QLabel(peerName_, contact);
  contactName_->setObjectName("contactName");
  statusLabel_ = new QLabel("连接中…", contact);
  statusLabel_->setObjectName("contactSub");
  cc->addWidget(contactName_);
  cc->addWidget(statusLabel_);
  cl->addWidget(contactAvatar_);
  cl->addLayout(cc, 1);
  sv->addWidget(contact);
  sv->addStretch(1);
  root->addWidget(sidebar);

  // ===== 右侧聊天面板 =====
  auto* rightPane = new QWidget(central);
  rightPane->setObjectName("rightPane");
  auto* rv = new QVBoxLayout(rightPane);
  rv->setContentsMargins(0, 0, 0, 0);
  rv->setSpacing(0);

  // 顶栏：对端名字
  auto* chatHeader = new QFrame(rightPane);
  chatHeader->setObjectName("chatHeader");
  auto* chl = new QHBoxLayout(chatHeader);
  chl->setContentsMargins(20, 14, 20, 14);
  headerTitle_ = new QLabel(peerName_, chatHeader);
  headerTitle_->setObjectName("chatTitle");
  chl->addWidget(headerTitle_);
  chl->addStretch(1);
  rv->addWidget(chatHeader);

  // 指标条
  auto* metricBar = new QFrame(rightPane);
  metricBar->setObjectName("metricBar");
  auto* ml = new QHBoxLayout(metricBar);
  ml->setContentsMargins(20, 8, 20, 8);
  ml->setSpacing(22);
  metricLat_ = new QLabel("字幕延迟: 暂无样本", metricBar);
  metricErr_ = new QLabel("标注误差: 暂无样本", metricBar);
  metricVis_ = new QLabel("可见延迟: 暂无样本", metricBar);
  metricLat_->setObjectName("metric");
  metricErr_->setObjectName("metric");
  metricVis_->setObjectName("metric");
  ml->addWidget(metricLat_);
  ml->addWidget(metricErr_);
  ml->addWidget(metricVis_);
  ml->addStretch(1);
  rv->addWidget(metricBar);

  // 聊天消息滚动区
  chatScroll_ = new QScrollArea(rightPane);
  chatScroll_->setObjectName("chatScroll");
  chatScroll_->setWidgetResizable(true);
  chatScroll_->setFrameShape(QFrame::NoFrame);
  chatScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  chatBody_ = new QWidget();
  chatBody_->setObjectName("chatBody");
  messagesLayout_ = new QVBoxLayout(chatBody_);
  messagesLayout_->setContentsMargins(20, 16, 20, 16);
  messagesLayout_->setSpacing(14);
  messagesLayout_->addStretch(1);  // 末尾弹簧：消息从顶部往下排
  chatScroll_->setWidget(chatBody_);
  rv->addWidget(chatScroll_, 1);

  // 底部输入栏：说话 + 标注
  auto* inputBar = new QFrame(rightPane);
  inputBar->setObjectName("inputBar");
  auto* il = new QHBoxLayout(inputBar);
  il->setContentsMargins(16, 12, 16, 12);
  il->setSpacing(10);
  talkButton_ = new QPushButton("开始说话", inputBar);
  talkButton_->setObjectName("talk");
  talkButton_->setMinimumHeight(40);
  talkButton_->setMinimumWidth(120);
  talkButton_->setCursor(Qt::PointingHandCursor);
  talkButton_->setCheckable(true);
  il->addWidget(talkButton_);
  noteEdit_ = new QLineEdit(inputBar);
  noteEdit_->setPlaceholderText("输入标注，回车或点发送");
  noteEdit_->setMinimumHeight(40);
  il->addWidget(noteEdit_, 1);
  noteButton_ = new QPushButton("发送标注", inputBar);
  noteButton_->setObjectName("primary");
  noteButton_->setMinimumHeight(40);
  noteButton_->setCursor(Qt::PointingHandCursor);
  il->addWidget(noteButton_);
  rv->addWidget(inputBar);

  root->addWidget(rightPane, 1);

  central->setStyleSheet(R"(
    QWidget#central, QWidget#rightPane { background:#FFFFFF; }
    QFrame#sidebar { background:#F7F8FA; border-right:1px solid #E5E6EB; }
    QLabel#sideTitle {
      font-size:15px; font-weight:600; color:#1F2329;
      padding:16px 16px 10px 16px;
    }
    QFrame#contact { background:#EAEEF5; }
    QLabel#contactName { font-size:14px; font-weight:500; color:#1F2329; }
    QLabel#contactSub { font-size:12px; color:#8F959E; }
    QFrame#chatHeader { background:#FFFFFF; border-bottom:1px solid #EEF0F3; }
    QLabel#chatTitle { font-size:16px; font-weight:600; color:#1F2329; }
    QFrame#metricBar { background:#FAFBFC; border-bottom:1px solid #EEF0F3; }
    QLabel#metric { color:#8F959E; font-size:12px; }
    QScrollArea#chatScroll, QWidget#chatBody { background:#F5F6F7; }
    QLabel#avatar {
      background:#C9CDD4; color:#FFFFFF; border-radius:18px;
      font-size:14px; font-weight:600;
    }
    QLabel#msgName { color:#8F959E; font-size:12px; }
    QLabel#bubbleMe {
      background:#3370FF; color:#FFFFFF; border-radius:10px;
      padding:9px 13px; font-size:14px;
    }
    QLabel#bubblePeer {
      background:#FFFFFF; color:#1F2329; border:1px solid #ECECEC;
      border-radius:10px; padding:9px 13px; font-size:14px;
    }
    QLabel#sysPill {
      background:#E3E6EB; color:#6B7280; font-size:12px;
      border-radius:11px; padding:4px 12px;
    }
    QFrame#inputBar { background:#FFFFFF; border-top:1px solid #EEF0F3; }
    QLineEdit {
      background:#F2F3F5; border:1px solid #E5E6EB; border-radius:8px;
      padding:0 12px; font-size:14px; color:#1F2329;
    }
    QLineEdit:focus { border:1px solid #3370FF; background:#FFFFFF; }
    QPushButton#primary {
      background:#3370FF; color:#FFFFFF; border:none;
      border-radius:8px; padding:0 20px; font-size:14px; font-weight:500;
    }
    QPushButton#primary:hover { background:#245BDB; }
    QPushButton#primary:pressed { background:#1D4FC4; }
    QPushButton#primary:disabled { background:#BBD0FF; color:#FFFFFF; }
    QPushButton#talk {
      background:#FFFFFF; color:#3370FF; border:1px solid #3370FF;
      border-radius:8px; font-size:14px; font-weight:600;
    }
    QPushButton#talk:hover { background:#EBF1FF; }
    QPushButton#talk:checked { background:#3370FF; color:#FFFFFF; border:none; }
    QPushButton#talk:disabled { color:#C9CDD4; border-color:#E5E6EB; background:#FFFFFF; }
  )");

  setCentralWidget(central);

  // 创建后端并接线（先连信号，再 start，避免漏事件）。
  backend_ = new Backend(this);
  connect(backend_, &Backend::stateEvent, this, &MainWindow::onState,
          Qt::QueuedConnection);
  connect(backend_, &Backend::subtitleEvent, this, &MainWindow::onSubtitle,
          Qt::QueuedConnection);
  connect(backend_, &Backend::markEvent, this, &MainWindow::onMark,
          Qt::QueuedConnection);
  connect(backend_, &Backend::orphanEvent, this, &MainWindow::onOrphan,
          Qt::QueuedConnection);

  connect(talkButton_, &QPushButton::toggled, this, &MainWindow::onTalkToggled);
  connect(noteButton_, &QPushButton::clicked, this, &MainWindow::onSendNote);
  connect(noteEdit_, &QLineEdit::returnPressed, this, &MainWindow::onSendNote);

  if (!backend_->start(id, host, port, audioPath)) {
    statusLabel_->setText("引擎启动失败");
    talkButton_->setEnabled(false);
    noteButton_->setEnabled(false);
    appendSystem("引擎启动失败，请检查信令服务器与模型路径");
    return;
  }

  isOfferer_ = backend_->isOfferer();
  if (!isOfferer_) {
    // B 端没有本地麦克风控制，禁用说话按钮。
    talkButton_->setEnabled(false);
    talkButton_->setText("接收方（B）");
  }
  statusLabel_->setText("等待对端…");
  appendSystem(QString("已连接信令 · 你是%1，正在等待 %2 加入…")
                   .arg(isOfferer_ ? "讲话方 A" : "接收方 B")
                   .arg(peerName_));

  // 指标定时刷新。
  metricTimer_ = new QTimer(this);
  connect(metricTimer_, &QTimer::timeout, this, &MainWindow::refreshMetrics);
  metricTimer_->start(500);
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent* e) {
  if (metricTimer_) metricTimer_->stop();
  if (backend_) backend_->stop();
  e->accept();
}

// 追加一条聊天气泡。mine=true 靠右蓝色，否则靠左白色，左/右带灰色头像。
void MainWindow::appendMessage(bool mine, const QString& name,
                               const QString& bodyHtml, const QString& footer) {
  auto* row = new QWidget(chatBody_);
  auto* h = new QHBoxLayout(row);
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(8);

  auto* avatar = makeAvatar(name, row);

  auto* col = new QWidget(row);
  col->setMaximumWidth(460);
  auto* v = new QVBoxLayout(col);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(3);

  auto* nameLbl = new QLabel(name, col);
  nameLbl->setObjectName("msgName");
  nameLbl->setAlignment(mine ? Qt::AlignRight : Qt::AlignLeft);

  QString html = bodyHtml;
  if (!footer.isEmpty()) {
    html += QString(
                "<div style='margin-top:5px; font-size:11px; color:%1;'>%2</div>")
                .arg(mine ? "#D6E2FF" : "#A9AEB8")
                .arg(footer.toHtmlEscaped());
  }
  auto* bubble = new QLabel(html, col);
  bubble->setObjectName(mine ? "bubbleMe" : "bubblePeer");
  bubble->setTextFormat(Qt::RichText);
  bubble->setWordWrap(true);
  bubble->setMaximumWidth(440);
  bubble->setTextInteractionFlags(Qt::TextSelectableByMouse);

  v->addWidget(nameLbl);
  v->addWidget(bubble);

  if (mine) {
    h->addStretch(1);
    h->addWidget(col);
    h->addWidget(avatar, 0, Qt::AlignTop);
  } else {
    h->addWidget(avatar, 0, Qt::AlignTop);
    h->addWidget(col);
    h->addStretch(1);
  }

  messagesLayout_->insertWidget(messagesLayout_->count() - 1, row);
  scrollToBottom();
}

void MainWindow::appendSystem(const QString& text) {
  auto* row = new QWidget(chatBody_);
  auto* h = new QHBoxLayout(row);
  h->setContentsMargins(0, 2, 0, 2);
  auto* pill = new QLabel(text, row);
  pill->setObjectName("sysPill");
  pill->setAlignment(Qt::AlignCenter);
  h->addStretch(1);
  h->addWidget(pill);
  h->addStretch(1);
  messagesLayout_->insertWidget(messagesLayout_->count() - 1, row);
  scrollToBottom();
}

void MainWindow::scrollToBottom() {
  // 布局尺寸要在事件循环里更新完才能拿到正确的 maximum。
  QTimer::singleShot(0, this, [this]() {
    auto* bar = chatScroll_->verticalScrollBar();
    bar->setValue(bar->maximum());
  });
}

void MainWindow::onState(QString line) {
  // 解析对端名字："[peer] B is online"。
  if (line.contains("is online")) {
    const int a = line.indexOf("] ");
    const int b = line.indexOf(" is online");
    if (a >= 0 && b > a + 2) {
      peerName_ = line.mid(a + 2, b - (a + 2)).trimmed();
      contactName_->setText(peerName_);
      contactAvatar_->setText(peerName_.left(1).toUpper());
      headerTitle_->setText(peerName_);
    }
    statusLabel_->setText("在线");
    appendSystem(QString("%1 已加入会话").arg(peerName_));
    return;
  }
  if (line.contains("left")) {
    statusLabel_->setText("离线");
    appendSystem("对方已离开会话");
    return;
  }
  if (line.startsWith("<peer> ")) {
    appendMessage(false, peerName_,
                  QString("<span>%1</span>")
                      .arg(line.mid(7).toHtmlEscaped()),
                  QString());
    return;
  }
  if (line.contains("[engine] started")) {
    statusLabel_->setText("等待对端…");
    return;
  }
  if (line.contains("connected", Qt::CaseInsensitive)) {
    statusLabel_->setText("已连接");
  }
}

void MainWindow::onSubtitle(QString text, qint64 startMs, qint64 endMs,
                            qint64 latencyMs, bool remote, QStringList markLines) {
  (void)remote;
  // 讲话方恒为 A：本端是 A 则靠右（我说的），否则靠左（对端 A 说的）。
  const bool mine = isOfferer_;
  const QString name = mine ? ownName_ : peerName_;

  QString body = QString("<span>%1</span>").arg(text.toHtmlEscaped());
  const QString chipColor = mine ? "#E8EFFF" : "#3370FF";
  for (const QString& mk : markLines) {
    body += QString("<div style='margin-top:4px; font-size:12px; color:%1;'>%2</div>")
                .arg(chipColor)
                .arg(mk.toHtmlEscaped());
  }

  QString footer;
  if (startMs > 0) {
    const QString t1 = QDateTime::fromMSecsSinceEpoch(startMs).toString("HH:mm:ss");
    const QString t2 = QDateTime::fromMSecsSinceEpoch(endMs).toString("HH:mm:ss");
    footer = QString("%1–%2 · 延迟%3ms").arg(t1, t2).arg(latencyMs);
  } else {
    footer = QString("延迟%1ms").arg(latencyMs);
  }
  if (latencyMs > 1500) footer += " ⚠";

  appendMessage(mine, name, body, footer);
}

void MainWindow::onMark(QString text, qint64 visMs) {
  // OnMark 只在「接收方」触发，所以这一定是对端发来的标注。
  QString footer = QString("标注 · 可见延迟%1ms").arg(visMs);
  if (visMs > 300) footer += " ⚠";
  appendMessage(false, peerName_,
                QString("<span>📌 %1</span>").arg(text.toHtmlEscaped()), footer);
}

void MainWindow::onOrphan(QString text) {
  appendMessage(false, peerName_,
                QString("<span>📌 %1</span>").arg(text.toHtmlEscaped()),
                "标注（未对齐到字幕）");
}

void MainWindow::onTalkToggled(bool on) {
  if (!isOfferer_) return;
  backend_->setTalking(on);
  talkButton_->setText(on ? "说话中（点击停止）" : "开始说话");
}

void MainWindow::onSendNote() {
  const QString text = noteEdit_->text().trimmed();
  if (text.isEmpty()) return;
  backend_->sendNote(text);
  noteEdit_->clear();
  // 本地回显自己发出的标注（引擎不会把自己的标注回调回来）。
  appendMessage(true, ownName_,
                QString("<span>📌 %1</span>").arg(text.toHtmlEscaped()),
                "标注（已发送）");
}

// 平均/峰值，超预算标红。
static QString metricText(const char* name, int64_t count, int64_t sum,
                          int64_t mx, int64_t budget) {
  if (count == 0) return QString("%1: 暂无样本").arg(name);
  const int64_t avg = sum / count;
  return QString("%1: 均%2 / 峰%3 ms (n=%4)%5")
      .arg(name)
      .arg(avg)
      .arg(mx)
      .arg(count)
      .arg(avg <= budget ? "" : " ⚠");
}

void MainWindow::refreshMetrics() {
  const AudiosubMetrics m = backend_->metrics();
  metricLat_->setText(
      metricText("字幕延迟", m.lat_count, m.lat_sum, m.lat_max, 1500));
  metricErr_->setText(
      metricText("标注误差", m.err_count, m.err_sum, m.err_max, 500));
  metricVis_->setText(
      metricText("可见延迟", m.vis_count, m.vis_sum, m.vis_max, 300));
}
