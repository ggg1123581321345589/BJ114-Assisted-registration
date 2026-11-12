#include "native_service.h"
#include "booking_service.h"
#include "booking_calendar.h"
#include "booking_table_tools.h"
#include <QtWidgets>
#include <QJsonDocument>
#include <QSaveFile>
#include <QThread>
#include <QUuid>
#include <memory>
#include <atomic>
#include <algorithm>
using namespace native;
static bool writeJson(const QString &path,const QJsonObject &obj) {
 QSaveFile f(path);if(!f.open(QIODevice::WriteOnly))return false;
 f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));return f.commit();
}
static QStringList csvLine(const QString &line) {
 QStringList out;QString value;bool quote=false;
 for(int i=0;i<line.size();++i){QChar c=line[i];if(c=='"'){if(quote&&i+1<line.size()&&line[i+1]=='"'){value+='"';++i;}else quote=!quote;}else if(c==','&&!quote){out<<value;value.clear();}else value+=c;}out<<value;return out;
}
static QString queryTimeForDisplay(const QString &value) {
 if(!QRegularExpression(QStringLiteral("(?:Z|[+-][0-9]{2}:[0-9]{2})$")).match(value).hasMatch())return QString::fromUtf8("未知");
 auto time=QDateTime::fromString(value,Qt::ISODateWithMs);if(!time.isValid())time=QDateTime::fromString(value,Qt::ISODate);
 return time.isValid()?time.toOffsetFromUtc(8*60*60).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")):QString::fromUtf8("未知");
}
class Worker final:public QObject {
 Q_OBJECT
 QString root_;int slot_;quint64 epoch_=0;std::unique_ptr<Service> service_;std::unique_ptr<BookingService> booking_;
 std::shared_ptr<std::atomic_bool> cancel_,publicCancel_;
public:
 Worker(QString root,std::shared_ptr<std::atomic_bool> cancel,int slot=0,std::shared_ptr<std::atomic_bool> publicCancel={}):root_(std::move(root)),slot_(slot),cancel_(std::move(cancel)),publicCancel_(publicCancel?std::move(publicCancel):std::make_shared<std::atomic_bool>(false)){}
 // Called only through this object's owner thread. A fresh token cannot revive old work.
 void resetQuery(int slot,quint64 epoch,std::shared_ptr<std::atomic_bool> token){
  if(slot!=slot_)return;if(service_)service_->close();service_.reset();publicCancel_=std::move(token);epoch_=epoch;emit queryReset(slot_,epoch_);
 }
public slots:
 void execute(QString op,QJsonObject params){
  const int slot=params.value("_connection_slot").toInt(0);if(slot!=slot_)return;
  const bool booking=op.startsWith("booking_");if(booking&&slot_!=0)return;
  const quint64 epoch=quint64(params.value("_connection_epoch").toDouble(double(epoch_)));
  if(!booking&&epoch!=epoch_)return;
  const auto token=booking?cancel_:publicCancel_;QJsonObject result;
  try{
   if(token->load())throw Failure("cancelled_by_user");
   if(booking){if(!booking_)booking_=std::make_unique<BookingService>(root_);result=booking_->execute(op,params,token.get());}
   else if(op=="crawl_all_departments"){if(service_){service_->close();service_.reset();}result=Service::crawlAllDepartments(root_,token.get(),params.value("workers").toInt(4));}
   else{if(!service_)service_=std::make_unique<Service>(root_);result=service_->execute(op,params,token.get());}
  }catch(const std::exception&e){result={{"status",token->load()?"cancelled":"failed"},{"failure",QString::fromUtf8(e.what())},{"message",QString::fromUtf8("任务未完成，请检查输入或稍后重新操作。")},{"rows",QJsonArray()},{"departments",QJsonArray()}};}
  result["_connection_slot"]=slot_;result["_connection_epoch"]=double(epoch);result["_background"]=params.value("_background").toBool();result["_request_id"]=params.value("_request_id");result["_batch_id"]=params.value("_batch_id");result["_date_key"]=params.value("_date_key");emit completed(op,result);
 }
 void shutdown(){publicCancel_->store(true);if(service_)service_->close();service_.reset();if(booking_)booking_->close();booking_.reset();emit stopped();}
signals:void completed(QString operation,QJsonObject result);void queryReset(int slot,quint64 epoch);void stopped();
};
class Window final:public QMainWindow {
 friend class UiProbe;
 friend class BookingUiProbe;
 Q_OBJECT
 QString root_,hospital_,jobHospital_;QJsonArray departments_;QJsonObject last_;
 QThread thread_,spareThread_;Worker *worker_=nullptr,*spareWorker_=nullptr;std::shared_ptr<std::atomic_bool> cancel_=std::make_shared<std::atomic_bool>(false);
 struct QuerySlot {
  bool ready=false,warming=false,failed=false,resetting=false,warmAfterReset=false,background=false,warmAttempted=false;
  int uses=0,api=0,http=0,recoveryFailures=0; qint64 reportedAgeMs=0,recoveryAfterMs=0;QElapsedTimer observed,warmClock;
  quint64 epoch=0,requestId=0,warmBatch=0,queryBatch=0;QString hospital,dateKey;bool queryInFlight=false;
  std::shared_ptr<std::atomic_bool> cancel=std::make_shared<std::atomic_bool>(false);
 } querySlots_[16];
 QThread *queryThreads_[16]={};Worker *queryWorkers_[16]={};int queryConnectionCount_=7;quint64 queryPoolGeneration_=0;
 int activeQuerySlot_=0;quint64 queryEpoch_=0,queryRequestId_=0,queryBatchId_=0;int batchQueryHttp_=0,batchConnectionHttp_=0;bool batchLegacy_=false;QSet<QString> legacyQueryHospitals_;QStringList pendingQueryDates_;QSet<QString> finishedQueryDates_;QMap<QString,QJsonArray> batchRowsByDate_;QMap<QString,int> batchDateRetries_;int batchMaxInFlight_=0,batchPlanHttp_=0;double batchPlanMs_=0,batchPrepareMs_=0;QElapsedTimer batchPlanTimer_;bool batchPlanReady_=false,batchPlanInFlight_=false;QMap<QString,QJsonObject> batchCalendarByDate_;bool batchAborting_=false;QJsonObject batchFailure_;QJsonObject batchContext_;QString batchNonce_;int selectionPreparedHttp_=0,batchPreparedHttpBeforeClick_=0;double selectionPreparedMs_=0,batchPreparedMsBeforeClick_=0;
 QLineEdit *search_;QTableWidget *hospitals_,*rows_;QComboBox *department_;QDateEdit *date_; // Hidden compatibility anchor; users select dates on calendar_.
  BookingCalendar *calendar_;BookingTableTools *hospitalTools_,*rowTools_;
  QList<QDate> queryDates_;QStringList queryNotices_;QJsonArray queryRows_;int queryDateIndex_=0,budgetRetryForDate_=0,sessionHttpUsed_=0;bool batchActive_=false,waitingConnection_=false;
 QPushButton *load_,*refresh_,*crawl_,*query_,*stop_,*prepareConnection_;QCheckBox *autoLoad_,*repeat_;QSpinBox *interval_,*crawlWorkers_,*connectionThreads_;QLabel *poolStatus_,*connection_,*status_,*performance_;QPlainTextEdit *log_;
 QTimer poolRecoveryTimer_;QElapsedTimer poolRecoveryClock_;bool poolRecoveryEnabled_=false,poolInitialPrimeDone_=false;int poolRecoveryFailures_=0; qint64 poolRecoveryAfterMs_=0;int poolRecoveryCursor_=0;
 QTimer debounce_,repeatTimer_,warmExpiry_,elapsedTimer_,catalogProgressTimer_,budgetReconnectTimer_;bool budgetReconnectCredit_=false,budgetReconnectPending_=false,automaticWarm_=false;QElapsedTimer jobTimer_,selectionTimer_,warmTimer_,sessionObserved_,userActivity_;qint64 sessionReportedAgeMs_=0;int sessionApiUsed_=0;bool renewalPending_=false,renewalCredit_=true;
 bool busy_=false,warming_=false,ready_=false,closing_=false,pendingLoad_=false,pendingForce_=false,lastQuerySuccess_=false;QString operation_;
 QLineEdit *phone_,*imageCode_,*smsCode_;QLabel *captchaImage_,*loginState_,*bookingSummary_;
 QPushButton *getCaptcha_,*sendSms_,*login_,*logout_,*loadPatients_,*previewBooking_,*submitBooking_;
 QComboBox *patient_,*card_;QCheckBox *terms_;QTimer smsTimer_,captchaExpiry_;int smsSeconds_=0;bool captchaLoaded_=false;
 bool loggedIn_=false,directSubmissionLocked_=false;QString bookingPreviewId_;QJsonObject bookingPreview_;QJsonArray bookingPatients_;
public:
 Window(QString root,bool warm):root_(std::move(root)){userActivity_.start();poolRecoveryClock_.start();Broker::prefetchPublicDns();
  setWindowTitle(QString::fromUtf8("辅助挂号软件 v1.1.1"));resize(1250,1000);
  auto *central=new QWidget;auto *scroll=new QScrollArea;scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);scroll->setWidget(central);setCentralWidget(scroll);auto *layout=new QVBoxLayout(central);
  auto *title=new QLabel(QString::fromUtf8("免责协议"));title->setStyleSheet("font-size:16px;font-weight:bold;");layout->addWidget(title);
  auto *disclaimer=new QLabel(QString::fromUtf8("本软件为非官方辅助工具，供用户本人查询号源并在人工确认后提交一次预约，不保证号源可用或挂号成功。请遵守医院及官方平台规则，核对就诊人、就诊卡、日期、费用及取退号要求；预约和支付结果以官方平台为准。验证码须由本人输入，遇到提交结果未知时请先核对官方订单，勿重复提交。请妥善保护个人信息，勿用于倒号、代抢或其他违规用途。"));disclaimer->setObjectName("disclaimerAgreement");disclaimer->setWordWrap(true);disclaimer->setTextFormat(Qt::PlainText);layout->addWidget(disclaimer);
  auto *accountBox=new QGroupBox(QString::fromUtf8("官方账号登录"));auto *account=new QGridLayout(accountBox);
  phone_=new QLineEdit;phone_->setEchoMode(QLineEdit::Normal);phone_->setMaxLength(11);phone_->setPlaceholderText(QString::fromUtf8("手机号"));phone_->setMaximumWidth(160);
  imageCode_=new QLineEdit;imageCode_->setMaxLength(4);imageCode_->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-9]{0,4}"),imageCode_));imageCode_->setPlaceholderText(QString::fromUtf8("图形验证码"));imageCode_->setMaximumWidth(130);
  captchaImage_=new QLabel(QString::fromUtf8("先获取图形码"));captchaImage_->setFixedSize(210,110);captchaImage_->setAlignment(Qt::AlignCenter);captchaImage_->setTextFormat(Qt::PlainText);captchaImage_->setToolTip(QString::fromUtf8("请填写图片中的4位数字；看不清可点击“获取 / 刷新图形码”。"));
  getCaptcha_=new QPushButton(QString::fromUtf8("获取 / 刷新图形码"));sendSms_=new QPushButton(QString::fromUtf8("发送短信验证码"));
  smsCode_=new QLineEdit;smsCode_->setMaxLength(8);smsCode_->setEchoMode(QLineEdit::Normal);smsCode_->setPlaceholderText(QString::fromUtf8("短信验证码"));smsCode_->setMaximumWidth(140);
  login_=new QPushButton(QString::fromUtf8("登录"));logout_=new QPushButton(QString::fromUtf8("退出登录"));loadPatients_=new QPushButton(QString::fromUtf8("刷新就诊人"));
  terms_=new QCheckBox(QString::fromUtf8("我已阅读并同意官方用户协议和隐私政策"));
  loginState_=new QLabel(QString::fromUtf8("未登录；验证码和会话仅保存在内存。"));loginState_->setWordWrap(true);loginState_->setTextFormat(Qt::PlainText);
  account->addWidget(phone_,0,0);account->addWidget(imageCode_,0,1);account->addWidget(captchaImage_,0,2);account->addWidget(getCaptcha_,0,3);account->addWidget(sendSms_,0,4);account->addWidget(smsCode_,0,5);account->addWidget(login_,0,6);
  account->addWidget(terms_,1,0,1,3);account->addWidget(logout_,1,5);account->addWidget(loadPatients_,1,6);account->addWidget(loginState_,2,0,1,7);layout->addWidget(accountBox);
  auto *warmRow=new QHBoxLayout;prepareConnection_=nullptr;connection_=new QLabel(QString::fromUtf8("站点连接：选择医院后后台静默准备，查询优先复用"));connection_->setObjectName("automaticConnectionStatus");warmRow->addWidget(connection_,1);connectionThreads_=new QSpinBox;connectionThreads_->setObjectName("queryConnectionThreads");connectionThreads_->setRange(1,16);connectionThreads_->setValue(queryConnectionCount_);connectionThreads_->setSuffix(QString::fromUtf8(" 个线程"));connectionThreads_->setToolTip(QString::fromUtf8("查询连接线程1–16；默认7，七天可同时查询。查询时并行准备连接；服务端拒绝时停止，不重发。"));warmRow->addWidget(new QLabel(QString::fromUtf8("连接线程")));warmRow->addWidget(connectionThreads_);poolStatus_=new QLabel;poolStatus_->setObjectName("queryConnectionPoolStatus");warmRow->addWidget(poolStatus_);layout->addLayout(warmRow);
  auto *queryPanel=new QHBoxLayout;auto *catalogPanel=new QVBoxLayout;queryPanel->addLayout(catalogPanel,3);calendar_=new BookingCalendar;queryPanel->addWidget(calendar_,2);layout->addLayout(queryPanel,3);
   search_=new QLineEdit;search_->setPlaceholderText(QString::fromUtf8("搜索医院名称、地区或编码"));catalogPanel->addWidget(search_);
  hospitals_=new QTableWidget(0,4);hospitals_->setHorizontalHeaderLabels({QString::fromUtf8("医院"),QString::fromUtf8("编码"),QString::fromUtf8("地区"),QString::fromUtf8("等级")});hospitals_->setSelectionBehavior(QAbstractItemView::SelectRows);hospitals_->setSelectionMode(QAbstractItemView::SingleSelection);hospitals_->setEditTriggers(QAbstractItemView::NoEditTriggers);hospitals_->horizontalHeader()->setStretchLastSection(true);hospitals_->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Stretch);hospitals_->setColumnWidth(1,110);hospitals_->setColumnWidth(2,75);hospitals_->setColumnWidth(3,65);hospitals_->setMinimumHeight(140);catalogPanel->addWidget(hospitals_,2);
  auto *form=new QGridLayout;department_=new QComboBox;department_->addItem(QString::fromUtf8("请选择医院并加载科室"));load_=new QPushButton(QString::fromUtf8("加载科室"));refresh_=new QPushButton(QString::fromUtf8("刷新科室"));form->addWidget(new QLabel(QString::fromUtf8("科室")),0,0);form->addWidget(department_,0,1);form->addWidget(load_,0,2);form->addWidget(refresh_,0,3);date_=new QDateEdit(QDate::currentDate(),this);date_->setDisplayFormat("yyyyMMdd");date_->hide();form->addWidget(new QLabel(QString::fromUtf8("查询日期")),1,0);auto *dateHint=new QLabel(QString::fromUtf8("在右侧月历点击多选日期；查询后显示当天医生。"));dateHint->setWordWrap(true);form->addWidget(dateHint,1,1,1,3);autoLoad_=new QCheckBox(QString::fromUtf8("选中医院后自动加载科室"));autoLoad_->setChecked(true);form->addWidget(autoLoad_,2,1);crawl_=new QPushButton(QString::fromUtf8("采集全院科室（断点续跑）"));form->addWidget(crawl_,2,2,1,2);crawlWorkers_=new QSpinBox;crawlWorkers_->setRange(1,4);crawlWorkers_->setValue(4);crawlWorkers_->setSuffix(QString::fromUtf8(" 个线程"));form->addWidget(new QLabel(QString::fromUtf8("全院采集并发")),3,2);form->addWidget(crawlWorkers_,3,3);catalogPanel->addLayout(form);
  auto *buttons=new QHBoxLayout;query_=new QPushButton(QString::fromUtf8("查询实时号源"));stop_=new QPushButton(QString::fromUtf8("停止"));repeat_=new QCheckBox(QString::fromUtf8("重复刷新"));interval_=new QSpinBox;interval_->setRange(60,3600);interval_->setValue(60);interval_->setSuffix(QString::fromUtf8(" 秒"));buttons->addWidget(query_);buttons->addWidget(stop_);buttons->addStretch();buttons->addWidget(repeat_);buttons->addWidget(interval_);layout->addLayout(buttons);
  status_=new QLabel(QString::fromUtf8("科室读取本地快照；首次使用请先运行一次全院科室采集。实时号源仅在查询时联网。"));status_->setTextFormat(Qt::PlainText);status_->setWordWrap(true);layout->addWidget(status_);performance_=new QLabel;layout->addWidget(performance_);
  rows_=new QTableWidget(0,7);rows_->setHorizontalHeaderLabels({QString::fromUtf8("日期"),QString::fromUtf8("科室"),QString::fromUtf8("医生"),QString::fromUtf8("时段"),QString::fromUtf8("剩余号源"),QString::fromUtf8("状态"),QString::fromUtf8("查询时间（北京时间）")});rows_->setEditTriggers(QAbstractItemView::NoEditTriggers);rows_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);rows_->horizontalHeader()->setSectionResizeMode(6,QHeaderView::ResizeToContents);rows_->setMinimumHeight(150);layout->addWidget(rows_,2);log_=new QPlainTextEdit;log_->setReadOnly(true);log_->setMaximumBlockCount(200);log_->setMaximumHeight(65);layout->addWidget(log_);
  rows_->setSelectionBehavior(QAbstractItemView::SelectRows);rows_->setSelectionMode(QAbstractItemView::SingleSelection);
  auto *bookingBox=new QGroupBox(QString::fromUtf8("人工确认挂号"));auto *bookingLayout=new QGridLayout(bookingBox);
  patient_=new QComboBox;patient_->addItem(QString::fromUtf8("请先登录并加载就诊人"));card_=new QComboBox;card_->addItem(QString::fromUtf8("请选择就诊卡"));
  previewBooking_=new QPushButton(QString::fromUtf8("预览选中号源"));submitBooking_=new QPushButton(QString::fromUtf8("直接挂号（提交一次）"));previewBooking_->hide();
  bookingSummary_=new QLabel(QString::fromUtf8("先登录，选择号源、就诊人和就诊卡，点击直接挂号并确认；程序会自动核验后提交一次。"));bookingSummary_->setWordWrap(true);bookingSummary_->setTextFormat(Qt::PlainText);
  bookingLayout->addWidget(new QLabel(QString::fromUtf8("就诊人")),0,0);bookingLayout->addWidget(patient_,0,1);bookingLayout->addWidget(new QLabel(QString::fromUtf8("就诊卡")),0,2);bookingLayout->addWidget(card_,0,3);bookingLayout->addWidget(submitBooking_,0,4,1,2);bookingLayout->addWidget(bookingSummary_,1,0,1,6);layout->addWidget(bookingBox);
  connect(getCaptcha_,&QPushButton::clicked,this,[this]{bookingAction("booking_captcha",{});});
  connect(sendSms_,&QPushButton::clicked,this,[this]{bookingAction("booking_sms",{{"phone",phone_->text().trimmed()},{"captcha",imageCode_->text().trimmed()}});});
  connect(login_,&QPushButton::clicked,this,[this]{if(!terms_->isChecked()){loginState_->setText(QString::fromUtf8("请先阅读并同意官方用户协议和隐私政策。"));return;}bookingAction("booking_login",{{"phone",phone_->text().trimmed()},{"sms_code",smsCode_->text().trimmed()}});});
  connect(logout_,&QPushButton::clicked,this,[this]{bookingAction("booking_logout",{});});connect(loadPatients_,&QPushButton::clicked,this,[this]{bookingAction("booking_patients",{});});
  connect(patient_,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int){populateBookingCards();});
  connect(card_,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int){updateBookingControls();});
  connect(rows_,&QTableWidget::itemSelectionChanged,this,[this]{clearBookingPreview();updateBookingControls();});
  connect(previewBooking_,&QPushButton::clicked,this,[this]{const auto row=selectedBookingRow();if(row.isEmpty())return;bookingAction("booking_preview",{{"row",row}});});
  connect(submitBooking_,&QPushButton::clicked,this,[this]{submitSelectedBooking();});
  connect(terms_,&QCheckBox::toggled,this,[this](bool){updateBookingControls();});
  for(auto *input:{phone_,imageCode_,smsCode_})connect(input,&QLineEdit::textChanged,this,[this](const QString &){updateBookingControls();});
  captchaExpiry_.setSingleShot(true);connect(&captchaExpiry_,&QTimer::timeout,this,[this]{clearCaptcha(QString::fromUtf8("图形码已过期，请刷新"));loginState_->setText(QString::fromUtf8("图形验证码已过期，请重新获取。"));updateBookingControls();});
  smsTimer_.setInterval(1000);connect(&smsTimer_,&QTimer::timeout,this,[this]{if(smsSeconds_>0)--smsSeconds_;if(smsSeconds_==0)smsTimer_.stop();updateBookingControls();});
  setStyleSheet("QPushButton{padding:6px 12px} QTableWidget{alternate-background-color:#f1f4f8} QLabel{padding:2px}");hospitals_->setAlternatingRowColors(true);rows_->setAlternatingRowColors(true);
  hospitalTools_=new BookingTableTools(hospitals_);rowTools_=new BookingTableTools(rows_);
   hospitalTools_->filtersChanged=[this]{const QSignalBlocker blocked(search_);search_->setText(hospitalTools_->globalSearchText());};rowTools_->filtersChanged=[this]{clearBookingPreview();updateBookingControls();};
   auto *tableHint=new QLabel(QString::fromUtf8("表头左键：升序 / 降序；右键：按列筛选、清除筛选。医生来自已查询结果，未查询日期不推测排班。"));tableHint->setWordWrap(true);layout->insertWidget(layout->indexOf(rows_),tableHint);
   hospitals_->setSortingEnabled(false);QFile file(QDir(root_).filePath("data/hospitals.csv"));if(file.open(QIODevice::ReadOnly)){QTextStream stream(&file);stream.setCodec("UTF-8");auto header=csvLine(stream.readLine());while(!stream.atEnd()){auto fields=csvLine(stream.readLine());if(fields.size()<header.size())continue;auto val=[&](const QString&key){int i=header.indexOf(key);return i>=0?fields[i]:QString();};int row=hospitals_->rowCount();hospitals_->insertRow(row);QStringList vs={val("hospital_name"),val("hospital_code"),val("area"),val("level")};for(int c=0;c<4;++c)hospitals_->setItem(row,c,new BookingTableItem(vs[c]));}}
  hospitals_->setSortingEnabled(true);hospitalTools_->refresh();
  for(int slot=0;slot<queryConnectionCount_;++slot)createQueryOwner(slot);
  connect(connectionThreads_,qOverload<int>(&QSpinBox::valueChanged),this,[this](int count){if(!configureQueryConnections(count)){const QSignalBlocker blocked(connectionThreads_);connectionThreads_->setValue(queryConnectionCount_);}});
  debounce_.setSingleShot(true);connect(&debounce_,&QTimer::timeout,this,[this]{loadDepartments(false);});warmExpiry_.setSingleShot(true);connect(&warmExpiry_,&QTimer::timeout,this,[this]{requireManualConnection();});catalogProgressTimer_.setInterval(1000);connect(&catalogProgressTimer_,&QTimer::timeout,this,[this]{QDir base(QDir(root_).filePath("output/hospital_availability_http_fast_gui/department_snapshot_staging"));const auto dirs=base.entryInfoList(QDir::Dirs|QDir::NoDotAndDotDot,QDir::Time);if(dirs.isEmpty())return;QFile f(QDir(dirs.first().absoluteFilePath()).filePath("progress.json"));if(!f.open(QIODevice::ReadOnly))return;const QByteArray progressBytes=f.readAll();f.close();QJsonParseError e;auto d=QJsonDocument::fromJson(progressBytes,&e);if(e.error!=QJsonParseError::NoError||!d.isObject())return;const auto p=d.object();status_->setText(QString::fromUtf8("全院科室采集：%1 / %2 家完成；当前 %3；进度已逐院保存。").arg(p.value("completed_count").toInt()).arg(p.value("hospital_count").toInt()).arg(p.value("current_hospital_code").toString()));});
  budgetReconnectTimer_.setSingleShot(true);connect(&budgetReconnectTimer_,&QTimer::timeout,this,[this]{const bool pending=budgetReconnectPending_;budgetReconnectPending_=false;if(pending&&!busy_&&!warming_&&!closing_&&!cancel_->load())startWarm(true);});repeatTimer_.setSingleShot(true);connect(&repeatTimer_,&QTimer::timeout,this,[this]{if(lastQuerySuccess_&&!busy_&&!warming_)query();});connect(repeat_,&QCheckBox::toggled,this,[this](bool yes){repeatTimer_.stop();if(yes&&lastQuerySuccess_)repeatTimer_.start(interval_->value()*1000);});connect(interval_,qOverload<int>(&QSpinBox::valueChanged),this,[this](int n){repeatTimer_.stop();if(repeat_->isChecked()&&lastQuerySuccess_)repeatTimer_.start(n*1000);});

   connect(search_,&QLineEdit::textChanged,this,[this](const QString&text){hospitalTools_->globalSearch(text);});
  connect(hospitals_,&QTableWidget::itemSelectionChanged,this,[this]{int r=hospitals_->currentRow();if(r<0){poolRecoveryEnabled_=false;resetQueryPool();hospital_.clear();departments_=QJsonArray();department_->clear();department_->addItem(QString::fromUtf8("请选择医院和科室"));calendar_->clearDoctors();rows_->setRowCount(0);updateControls();return;}resetQueryPool();cancelBudgetReconnect();clearBookingPreview();repeatTimer_.stop();lastQuerySuccess_=false;pendingLoad_=false;pendingForce_=false;noteUserActivity();hospital_=hospitals_->item(r,1)->text();selectionPreparedHttp_=0;selectionPreparedMs_=0;departments_=QJsonArray();department_->clear();department_->addItem(QString::fromUtf8("请选择科室"));calendar_->clearDoctors();rows_->setRowCount(0);selectionTimer_.start();cancel_->store(false);enablePoolRecovery();primeSelectedHospitalPool();updateControls();if(autoLoad_->isChecked())debounce_.start(100);});
  connect(load_,&QPushButton::clicked,this,[this]{loadDepartments(false);});connect(refresh_,&QPushButton::clicked,this,[this]{loadDepartments(true);});connect(crawl_,&QPushButton::clicked,this,&Window::crawlCatalog);connect(query_,&QPushButton::clicked,this,[this]{budgetReconnectCredit_=true;query();});connect(stop_,&QPushButton::clicked,this,&Window::stop);connect(department_,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{calendar_->clearDoctors();cancelBudgetReconnect();clearBookingPreview();if(department_->currentIndex()>0){noteUserActivity();}repeatTimer_.stop();lastQuerySuccess_=false;rows_->setRowCount(0);updateControls();});calendar_->selectionChanged=[this]{cancelBudgetReconnect();clearBookingPreview();const auto dates=calendar_->selectedDates();if(!dates.isEmpty()){const QSignalBlocker blocked(date_);date_->setDate(dates.first());}noteUserActivity();primeSelectedHospitalPool();repeatTimer_.stop();lastQuerySuccess_=false;rows_->setRowCount(0);updateControls();};
   connect(date_,&QDateEdit::dateChanged,this,[this](const QDate &date){calendar_->setSelectedDates({date});});
  connect(&elapsedTimer_,&QTimer::timeout,this,[this]{const auto &clock=warming_?warmTimer_:jobTimer_;if(clock.isValid()&&(busy_||warming_))performance_->setText((warming_?QString::fromUtf8("自动连接已等待 %1 秒（尚未发起号源查询）"):QString::fromUtf8("任务已等待 %1 秒（含队列、网络和界面处理）")).arg(clock.elapsed()/1000.0,0,'f',3));});elapsedTimer_.start(100);poolRecoveryTimer_.setInterval(1000);connect(&poolRecoveryTimer_,&QTimer::timeout,this,[this]{maintainQueryPool();});poolRecoveryTimer_.start();updateControls();Q_UNUSED(warm);
 }
 ~Window()override{cancel_->store(true);for(auto &slot:querySlots_)slot.cancel->store(true);emit shutdown();for(int slot=0;slot<16;++slot)if(queryThreads_[slot]&&queryThreads_[slot]->isRunning())queryThreads_[slot]->wait();}
 QJsonObject bookingUiState()const{return {{"booking_enabled",true},{"login_controls_present",bool(phone_&&imageCode_&&smsCode_&&login_&&sendSms_)},{"patient_controls_present",bool(patient_&&card_&&previewBooking_&&submitBooking_)},{"manual_connection_preparation",bool(prepareConnection_)},{"automatic_connection_renewal",true},{"automatic_connection_renewal_scope","hospital_selection_and_bounded_background_pool_recovery"},{"connection_pool_auto_recovery",true},{"connection_pool_recovery_enabled",poolRecoveryEnabled_},{"connection_pool_ready_target",poolReadyTarget()},{"connection_pool_recovery_parallel_max",2},{"connection_pool_retry_backoff_max_ms",60000},{"startup_public_network",false},{"selection_public_prewarm",true},{"selection_prewarm_background",true},{"query_connection_threads",queryConnectionCount_},{"query_running_threads",runningQueryThreads()},{"query_ready_connections",readyQueryConnections()},{"query_connection_threads_max",16},{"query_connection_threads_min",1},{"multi_date_calendar",true},{"calendar_doctor_annotations",true},{"table_header_sort_and_filter",true},{"manual_submit_confirmation",true},{"app_title",windowTitle()}};}
 void benchmarkSelect(const QString &code){for(int r=0;r<hospitals_->rowCount();++r)if(hospitals_->item(r,1)->text()==code){hospitals_->setCurrentCell(r,0);debounce_.stop();loadDepartments(false);return;}}
 bool benchmarkQuery(const QString &code,const QString &department,const QString &first,const QList<QDate> &dates,int threads){
  if(dates.isEmpty()||threads<1||threads>16||!configureQueryConnections(threads))return false;{const QSignalBlocker blocked(connectionThreads_);connectionThreads_->setValue(threads);}autoLoad_->setChecked(false);int hospitalRow=-1;for(int r=0;r<hospitals_->rowCount();++r)if(hospitals_->item(r,1)->text()==code){hospitalRow=r;break;}if(hospitalRow<0)return false;hospitals_->setCurrentCell(hospitalRow,0);debounce_.stop();const auto cached=Service::cacheOnly(root_,code);if(cached.isEmpty())return false;jobHospital_=code;completed("departments",cached);int index=-1;for(int i=1;i<department_->count();++i)if(department_->itemData(i).toString()==department&&(first.isEmpty()||department_->itemData(i,Qt::UserRole+1).toString()==first)){if(index>=0)return false;index=i;}if(index<0)return false;department_->setCurrentIndex(index);calendar_->setSelectedDates(dates);query();return batchActive_;
 }
 bool benchmarkPrimeHospital(const QString &code,const QList<QDate> &dates,int threads){
  if(dates.isEmpty()||threads<1||threads>16||!configureQueryConnections(threads))return false;{const QSignalBlocker blocked(connectionThreads_);connectionThreads_->setValue(threads);}autoLoad_->setChecked(false);calendar_->setSelectedDates(dates);benchmarkSelect(code);return hospital_==code;
 }
 QJsonObject selectionPrewarmState()const{return {{"ready",readyQueryConnections()},{"warming",warmingQueryConnections()},{"resetting",poolResetting()},{"target",poolReadyTarget()},{"auto_recovery",poolRecoveryEnabled_},{"completed_http",selectionPreparedHttp_},{"completed_prepare_ms_sum",selectionPreparedMs_},{"ui_hospitals_enabled",hospitals_->isEnabled()},{"ui_departments_enabled",department_->isEnabled()},{"ui_calendar_enabled",calendar_->isEnabled()}};}
signals:void work(QString operation,QJsonObject params);void shutdown();void resultRendered(QJsonObject);
private:
 QJsonObject selectedBookingRow()const{const int r=rows_->currentRow();return r>=0&&!rows_->isRowHidden(r)&&rows_->item(r,0)?rows_->item(r,0)->data(Qt::UserRole).toJsonObject():QJsonObject();}
 void clearBookingPreview(){bookingPreviewId_.clear();bookingPreview_=QJsonObject();}
  void clearCaptcha(const QString &message){captchaLoaded_=false;captchaExpiry_.stop();captchaImage_->clear();captchaImage_->setText(message);imageCode_->clear();}
 void populateBookingCards(int selectedCard=-1,bool preserveSelection=false){
  const QSignalBlocker blocked(card_);card_->clear();card_->addItem(QString::fromUtf8("请选择就诊卡"),-1);
  const QString id=patient_->currentData().toString();for(const auto &v:bookingPatients_){auto p=v.toObject();if(p.value("patient_id").toString()!=id)continue;for(const auto &c:p.value("cards").toArray()){auto x=c.toObject();card_->addItem(x.value("label").toString(QString::fromUtf8("就诊卡")),x.value("card_index").toInt(-1));}}
  if(preserveSelection){const int index=card_->findData(selectedCard);if(index>0)card_->setCurrentIndex(index);}else if(card_->count()==2)card_->setCurrentIndex(1);updateBookingControls();
 }
 void populateBookingPatients(const QJsonArray &patients){
  const QString selectedPatient=patient_->currentData().toString();const int selectedCard=card_->currentIndex()>0?card_->currentData().toInt():-1;
  bookingPatients_=patients;const QSignalBlocker blocked(patient_);patient_->clear();patient_->addItem(QString::fromUtf8("请选择就诊人"));
  for(const auto &v:patients){auto p=v.toObject();patient_->addItem(p.value("name").toString(QString::fromUtf8("就诊人")),p.value("patient_id").toString());}
  const int index=patient_->findData(selectedPatient);if(!selectedPatient.isEmpty()){if(index>0)patient_->setCurrentIndex(index);}else if(patient_->count()==2)patient_->setCurrentIndex(1);populateBookingCards(selectedCard,!selectedPatient.isEmpty()&&index>0);
 }
 void updateBookingControls(){
  if(!getCaptcha_)return;const bool idle=!busy_&&!warming_&&!closing_&&!batchActive_&&!foregroundPoolResetting();
  const bool phoneOk=QRegularExpression("^1[3-9][0-9]{9}$").match(phone_->text().trimmed()).hasMatch();const bool captchaOk=QRegularExpression("^[0-9]{4}$").match(imageCode_->text().trimmed()).hasMatch();const bool smsOk=QRegularExpression("^[0-9]{6}$").match(smsCode_->text().trimmed()).hasMatch();
  getCaptcha_->setEnabled(idle&&!loggedIn_);sendSms_->setEnabled(idle&&!loggedIn_&&terms_->isChecked()&&phoneOk&&captchaLoaded_&&captchaOk&&smsSeconds_==0);login_->setEnabled(idle&&!loggedIn_&&terms_->isChecked()&&phoneOk&&smsOk);logout_->setEnabled(idle&&loggedIn_);loadPatients_->setEnabled(idle&&loggedIn_);
  phone_->setEnabled(idle&&!loggedIn_);imageCode_->setEnabled(idle&&!loggedIn_);smsCode_->setEnabled(idle&&!loggedIn_);terms_->setEnabled(idle&&!loggedIn_);
  patient_->setEnabled(idle&&loggedIn_);card_->setEnabled(idle&&loggedIn_);
  const auto row=selectedBookingRow();const auto product=row.value("product_status");const bool available=row.value("booking_supported")!=QJsonValue(false)&&(product.toString()=="1"||(product.isDouble()&&product.toInt()==1));
  previewBooking_->setEnabled(idle&&loggedIn_&&available&&!row.value("booking_payload").toObject().isEmpty());
  submitBooking_->setEnabled(idle&&loggedIn_&&!directSubmissionLocked_&&available&&!row.value("booking_payload").toObject().isEmpty()&&!patient_->currentData().toString().isEmpty()&&card_->currentIndex()>0);
  sendSms_->setText(smsSeconds_>0?QString::fromUtf8("%1 秒后可重发").arg(smsSeconds_):QString::fromUtf8("发送短信验证码"));
 }
 void bookingAction(const QString &op,const QJsonObject &params){
  if(busy_||warming_||closing_||batchActive_||foregroundPoolResetting())return;cancelBudgetReconnect();if((op=="booking_sms"||op=="booking_login")&&!terms_->isChecked()){loginState_->setText(QString::fromUtf8("请先阅读并同意官方用户协议和隐私政策。"));return;}if(op=="booking_sms"&&!captchaLoaded_){loginState_->setText(QString::fromUtf8("请先获取有效图形验证码。"));return;}if(op=="booking_captcha"){clearCaptcha(QString::fromUtf8("正在加载图形码…"));getCaptcha_->setText(QString::fromUtf8("正在加载…"));}else if(op=="booking_sms")clearCaptcha(QString::fromUtf8("本次图形码已使用"));if(op!="booking_submit")clearBookingPreview();repeatTimer_.stop();debounce_.stop();repeat_->setChecked(false);cancel_->store(false);if(querySlots_[0].warming||(querySlots_[0].resetting&&querySlots_[0].warmAfterReset))resetQuerySlot(0);busy_=true;operation_=op;jobTimer_.start();
  if(op=="booking_preview"){clearBookingPreview();const auto row=params.value("row").toObject();bookingSummary_->setText(QString::fromUtf8("日期：%1  科室：%2  医生：%3  时段：%4\n已显示所选号源，正在官方核验；费用及可预约状态以核验结果为准。").arg(row.value("date").toString(),row.value("department_name").toString(),row.value("doctor_name").toString(),row.value("period").toString()));}loginState_->setText(QString::fromUtf8("正在处理，请稍候…"));emit work(op,params);updateControls();
 }
 void submitSelectedBooking(){
  if(!submitBooking_->isEnabled())return;cancelBudgetReconnect();const auto row=selectedBookingRow();if(row.isEmpty())return;
  const QString patientId=patient_->currentData().toString();const int cardIndex=card_->currentData().toInt();
  const QString summary=QString::fromUtf8("此操作会在官方核验通过后提交一次真实挂号订单。\n\n就诊人：%1\n就诊卡：%2\n医院：%3  日期：%4\n科室：%5  医生：%6  时段：%7\n\n费用以官方结算为准；请确认所选号源和就诊卡。若提交结果未知，请先在官方订单页核对，避免重复挂号。").arg(patient_->currentText(),card_->currentText(),row.value("booking_payload").toObject().value("hosCode").toString(),row.value("date").toString(),row.value("department_name").toString(),row.value("doctor_name").toString(),row.value("period").toString());
  if(QMessageBox::question(this,QString::fromUtf8("确认真实挂号"),summary,QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes)return;
  if(busy_||warming_||closing_||!loggedIn_||directSubmissionLocked_||selectedBookingRow()!=row||patient_->currentData().toString()!=patientId||card_->currentData().toInt()!=cardIndex)return;
  const QJsonObject params{{"row",row},{"patient_id",patientId},{"card_index",cardIndex},{"confirmed",true}};
  clearBookingPreview();directSubmissionLocked_=true;bookingSummary_->setText(QString::fromUtf8("正在自动核验所选号源和就诊卡，通过后提交一次；请勿重复操作。"));bookingAction("booking_direct",params);
 }
 void bookingCompleted(const QString &op,const QJsonObject &result){
  if(closing_)return;busy_=false;const QString state=result.value("status").toString();const bool ok=state=="complete"&&!(op=="booking_captcha"&&cancel_->load());
  if(result.contains("logged_in"))loggedIn_=result.value("logged_in").toBool();
  const QString message=result.value("message").toString(ok?QString::fromUtf8("操作完成。"):QString::fromUtf8("操作未完成，请检查输入或稍后重新操作。"));loginState_->setText(message);
  if(op=="booking_captcha"){getCaptcha_->setText(QString::fromUtf8("获取 / 刷新图形码"));clearCaptcha(ok?QString::fromUtf8("图形码读取失败，请刷新"):QString::fromUtf8("加载失败，请重新获取"));if(ok){QPixmap picture;if(picture.loadFromData(QByteArray::fromBase64(result.value("captcha_b64").toString().toLatin1()))){captchaImage_->setPixmap(picture.scaled(captchaImage_->contentsRect().size(),Qt::KeepAspectRatio,Qt::SmoothTransformation));captchaLoaded_=true;captchaExpiry_.start(180000);imageCode_->setFocus();}else loginState_->setText(QString::fromUtf8("验证码图片读取失败，请重新获取。"));}}else if(!ok||op=="booking_login"||op=="booking_logout")clearCaptcha(QString::fromUtf8("请重新获取图形码"));
  if(op=="booking_sms"&&ok){smsSeconds_=qMax(0,result.value("retry_after_seconds").toInt(60));if(smsSeconds_>0)smsTimer_.start();}
  if(op=="booking_login"){smsCode_->clear();imageCode_->clear();if(ok&&loggedIn_)loginState_->setText(result.value("authenticated").toBool()?QString::fromUtf8("已登录且已实名；可刷新就诊人并直接挂号。"):QString::fromUtf8("登录成功；账号实名状态以官方平台为准。"));}
  if(result.value("patients").isArray())populateBookingPatients(result.value("patients").toArray());
  if(op=="booking_preview"){clearBookingPreview();if(ok){bookingPreviewId_=result.value("preview_id").toString();bookingPreview_=result.value("preview").toObject();const auto p=bookingPreview_;bookingSummary_->setText(QString::fromUtf8("医院：%1  日期：%2  医生：%3 %4  时段：%5 %6\n费用：%7（以官方结算为准）\n取号：%8\n退号：%9").arg(p.value("hospital_code").toString(),p.value("date").toString(),p.value("doctorName").toString(),p.value("doctorTitle").toString(),p.value("regHalfView").toString(),p.value("visitRegHalfTime").toString(),p.value("totalFee").toVariant().toString(),p.value("takeTimeTips").toString(),p.value("refundTips").toString()));if(patient_->count()<=1||card_->count()<=1)bookingSummary_->setText(bookingSummary_->text()+QString::fromUtf8("\n当前缺少可用就诊人或就诊卡，请先在官方平台完善资料，再刷新就诊人。"));}else bookingSummary_->setText(message);}
  if(op=="booking_submit"||op=="booking_direct"){if(op=="booking_direct"&&!result.value("submit_attempted").toBool())directSubmissionLocked_=false;clearBookingPreview();if(state=="outcome_unknown"||result.value("outcome_unknown").toBool())bookingSummary_->setText(QString::fromUtf8("提交结果未知。请立即在官方订单页核对；本程序不会自动重发本次提交。"));else if(ok)bookingSummary_->setText(QString::fromUtf8("挂号提交成功，订单：%1。请在官方订单页核对状态、支付及取号要求。").arg(result.value("order_id").toVariant().toString()));else bookingSummary_->setText(message);}
  if(!loggedIn_){directSubmissionLocked_=false;clearBookingPreview();populateBookingPatients({});if(op=="booking_logout"){phone_->clear();smsCode_->clear();imageCode_->clear();captchaImage_->setText(QString::fromUtf8("先获取图形码"));smsTimer_.stop();smsSeconds_=0;}}
  if(op=="booking_preview"){const double uiMs=jobTimer_.isValid()?double(jobTimer_.nsecsElapsed())/1000000.0:0;log_->appendPlainText(QString::fromUtf8("预览官方核验：%1 ms；HTTP %2 次；医院规则缓存 %3。").arg(uiMs,0,'f',1).arg(result.value("request_count").toInt()).arg(result.value("hospital_rules_cache_hit").toBool()?QString::fromUtf8("命中"):QString::fromUtf8("未命中")));}updateControls();emit resultRendered(result);
 }
 qint64 sessionAgeMs()const{return sessionReportedAgeMs_+(sessionObserved_.isValid()?sessionObserved_.elapsed():0);}
 void noteUserActivity(){userActivity_.restart();renewalCredit_=true;}
  void cancelBudgetReconnect(){budgetReconnectTimer_.stop();budgetReconnectPending_=false;}
 // Each slot owns a Service on one owner thread; the GUI owns only its snapshot.
 QString queryHospital()const{return hospital_.isEmpty()?QStringLiteral("H01110005"):hospital_;}
 int poolReadyTarget()const{const int work=qMin(queryConnectionCount_,qMax(7,calendar_->selectedDates().size()));return qMin(queryConnectionCount_,work+(queryConnectionCount_>work?1:0));}
 bool foregroundPoolResetting()const{for(int slot=0;slot<queryConnectionCount_;++slot)if(querySlots_[slot].resetting&&!querySlots_[slot].background)return true;return false;}
 void enablePoolRecovery(){poolRecoveryEnabled_=true;poolInitialPrimeDone_=false;poolRecoveryFailures_=0;poolRecoveryAfterMs_=0;poolRecoveryCursor_=0;for(auto &q:querySlots_){q.recoveryFailures=0;q.recoveryAfterMs=0;}}
 void notePoolRecoveryFailure(int slot,const QJsonObject &result){
  if(!poolRecoveryEnabled_||closing_||cancel_->load()||querySlots_[slot].cancel->load()||result.value("failure")==QJsonValue("cancelled_by_user"))return;
  auto &q=querySlots_[slot];q.recoveryFailures=qMin(6,q.recoveryFailures+1);poolRecoveryFailures_=qMin(6,poolRecoveryFailures_+1);const int delay=qMin(60000,2000*(1<<(poolRecoveryFailures_-1)));q.recoveryAfterMs=poolRecoveryClock_.elapsed()+delay;poolRecoveryAfterMs_=qMax(poolRecoveryAfterMs_,q.recoveryAfterMs);
  log_->appendPlainText(QString::fromUtf8("连接池槽%1不可用：%2；后台恢复退避 %3 秒，失败业务不会重发。").arg(slot).arg(result.value("failure").toString(QStringLiteral("connection_not_ready"))).arg(delay/1000));
 }
 void maintainQueryPool(){
  if(!poolRecoveryEnabled_||closing_||hospital_.isEmpty()||cancel_->load()||busy_||batchActive_||warming_||pendingLoad_)return;
  const qint64 now=poolRecoveryClock_.elapsed();
  const auto healthy=[this](int slot){return querySlotReady(slot)&&querySlotAgeMs(slot)<80000&&querySlots_[slot].uses<3;};
  if(!querySlots_[activeQuerySlot_].queryInFlight&&!healthy(activeQuerySlot_)){
   int replacement=-1;for(int offset=1;offset<queryConnectionCount_;++offset){const int slot=(activeQuerySlot_+offset)%queryConnectionCount_;if(healthy(slot)){replacement=slot;break;}}
   if(replacement>=0){const int previous=activeQuerySlot_;activeQuerySlot_=replacement;mirrorActiveQuerySlot();connection_->setText(QString::fromUtf8("站点连接：已由线程%1接替线程%2；旧连接后台补充").arg(replacement+1).arg(previous+1));}
  }
  int engaged=0;for(int slot=0;slot<queryConnectionCount_;++slot){const auto &q=querySlots_[slot];if(q.hospital==hospital_&&(healthy(slot)||q.queryInFlight||q.warming||(q.resetting&&q.warmAfterReset)))++engaged;}
  int available=qMax(0,2-warmingQueryConnections());const int target=poolReadyTarget();
  if(now>=poolRecoveryAfterMs_)for(int pass=0;pass<2&&available>0&&engaged<target;++pass){
   const int first=poolRecoveryCursor_;
   for(int offset=0;offset<queryConnectionCount_&&available>0&&engaged<target;++offset){const int slot=(first+offset)%queryConnectionCount_;auto &q=querySlots_[slot];
    if(q.queryInFlight||q.warming||q.resetting||healthy(slot)||now<q.recoveryAfterMs)continue;
    if(pass==0&&(q.warmAttempted||q.ready||q.failed||q.observed.isValid()))continue;
    q.warmAttempted=false;startSlotWarm(slot,true);poolRecoveryCursor_=(slot+1)%queryConnectionCount_;++engaged;--available;
   }
  }
  mirrorActiveQuerySlot();updateControls();
 }

 bool poolResetting()const{for(int slot=0;slot<queryConnectionCount_;++slot)if(querySlots_[slot].resetting)return true;return false;}
 int warmingQueryConnections()const{int count=0;for(int slot=0;slot<queryConnectionCount_;++slot)if(querySlots_[slot].warming||(querySlots_[slot].resetting&&querySlots_[slot].warmAfterReset))++count;return count;}
 bool poolWarming()const{return warmingQueryConnections()>0;}
 bool queryOwnersRunning()const{for(int slot=0;slot<16;++slot)if(queryThreads_[slot]&&queryThreads_[slot]->isRunning())return true;return false;}
 int runningQueryThreads()const{int count=0;for(int slot=0;slot<queryConnectionCount_;++slot)if(queryThreads_[slot]&&queryThreads_[slot]->isRunning())++count;return count;}
 int readyQueryConnections()const{int count=0;for(int slot=0;slot<queryConnectionCount_;++slot)if(querySlotReady(slot))++count;return count;}
 void updatePoolStatus(){if(poolStatus_){poolStatus_->setText(QString::fromUtf8("配置 %1；运行线程 %2；连接就绪 %3；准备中 %4；维持目标 %5").arg(queryConnectionCount_).arg(runningQueryThreads()).arg(readyQueryConnections()).arg(warmingQueryConnections()).arg(poolReadyTarget()));poolStatus_->setToolTip(QString::fromUtf8("运行线程：已经启动的查询工作线程。连接就绪：可立即复用的 HTTP 会话。按所选日期维持连接，并在有空余线程时预留1个就绪备用；80秒或接近额度时后台轮换，100秒失效。后台补连最多并发2个；失败2至60秒退避。就绪表示可复用会话，不是实时TCP探活；查询中的连接暂不计就绪。"));}}
 void createQueryOwner(int slot){
  auto &q=querySlots_[slot];q.cancel=std::make_shared<std::atomic_bool>(false);q.epoch=++queryEpoch_;q.ready=q.warming=q.failed=q.resetting=q.warmAfterReset=q.warmAttempted=false;q.uses=q.api=q.http=0;q.observed.invalidate();
  QThread *t=slot==0?&thread_:slot==1?&spareThread_:new QThread(this);queryThreads_[slot]=t;auto *w=new Worker(root_,cancel_,slot,q.cancel);queryWorkers_[slot]=w;if(slot==0)worker_=w;if(slot==1)spareWorker_=w;
  w->moveToThread(t);connect(this,&Window::work,w,&Worker::execute);connect(this,&Window::shutdown,w,&Worker::shutdown);connect(w,&Worker::completed,this,&Window::completed);connect(w,&Worker::queryReset,this,&Window::queryResetCompleted);connect(w,&Worker::stopped,t,&QThread::quit,Qt::DirectConnection);connect(t,&QThread::finished,w,&QObject::deleteLater);connect(t,&QThread::finished,this,[this]{if(closing_&&!queryOwnersRunning())close();});t->setObjectName(QStringLiteral("queryConnectionOwner%1").arg(slot));t->start();
  // Establish the initial epoch on the owner before accepting a tagged operation.
  const quint64 epoch=q.epoch;const auto token=q.cancel;QMetaObject::invokeMethod(w,[w,slot,epoch,token]{w->resetQuery(slot,epoch,token);},Qt::QueuedConnection);
 }
 bool configureQueryConnections(int count){
  count=qBound(1,count,16);if(count==queryConnectionCount_)return true;
  if(busy_||batchActive_||warming_||poolWarming()||poolResetting()||closing_)return false;
  ++queryPoolGeneration_;cancelBudgetReconnect();ready_=false;warmExpiry_.stop();
  const int oldCount=queryConnectionCount_;
  for(int slot=count;slot<oldCount;++slot){auto &q=querySlots_[slot];q.cancel->store(true);q.epoch=++queryEpoch_;q.ready=false;auto *w=queryWorkers_[slot];auto *t=queryThreads_[slot];if(t&&t->isRunning()){QMetaObject::invokeMethod(w,[w]{w->shutdown();},Qt::QueuedConnection);t->wait();}queryWorkers_[slot]=nullptr;queryThreads_[slot]=nullptr;if(slot>=2)delete t;if(slot==1)spareWorker_=nullptr;}
  queryConnectionCount_=count;for(int slot=oldCount;slot<count;++slot)createQueryOwner(slot);activeQuerySlot_=0;resetQueryPool();
  {const QSignalBlocker blocked(connectionThreads_);connectionThreads_->setValue(count);}log_->appendPlainText(QString::fromUtf8("连接线程配置为 %1；每个就绪连接一次查询一个不同日期，连接逐步准备。").arg(count));primeSelectedHospitalPool();updateControls();return true;
 }
 int readySpareSlot()const{for(int offset=1;offset<queryConnectionCount_;++offset){const int slot=(activeQuerySlot_+offset)%queryConnectionCount_;if(querySlotReady(slot))return slot;}return -1;}
 int warmingSpareSlot()const{for(int offset=1;offset<queryConnectionCount_;++offset){const int slot=(activeQuerySlot_+offset)%queryConnectionCount_;const auto &q=querySlots_[slot];if((q.warming||(q.resetting&&q.warmAfterReset))&&q.hospital==queryHospital())return slot;}return -1;}
 void invalidateStandbys(int active){for(int slot=0;slot<queryConnectionCount_;++slot)if(slot!=active)resetQuerySlot(slot);}

 qint64 querySlotAgeMs(int slot)const{const auto &q=querySlots_[slot];return q.reportedAgeMs+(q.observed.isValid()?q.observed.elapsed():0);}
 bool querySlotReady(int slot)const{if(slot<0||slot>=queryConnectionCount_)return false;const auto &q=querySlots_[slot];const int required=batchActive_&&batchPlanReady_&&!batchLegacy_?1:3;return q.ready&&!q.queryInFlight&&!q.warming&&!q.failed&&!q.resetting&&q.hospital==queryHospital()&&q.observed.isValid()&&querySlotAgeMs(slot)<100000&&q.uses<4&&8-q.api>=required&&12-q.http>=required;}
 int querySlotCapacity(int slot)const{if(!querySlotReady(slot))return 0;const auto &q=querySlots_[slot];const int reserve=batchActive_&&batchPlanReady_&&!batchLegacy_?0:2;return qMax(0,qMin(4-q.uses,qMin(8-q.api-reserve,12-q.http-reserve)));}
 void mirrorActiveQuerySlot(){const auto &q=querySlots_[activeQuerySlot_];ready_=querySlotReady(activeQuerySlot_);warming_=!q.background&&(q.warming||(q.resetting&&q.warmAfterReset));sessionApiUsed_=q.api;sessionHttpUsed_=q.http;sessionReportedAgeMs_=q.reportedAgeMs;sessionObserved_=q.observed;warmExpiry_.stop();if(ready_)warmExpiry_.start(qMax(1,100000-int(querySlotAgeMs(activeQuerySlot_))));}
 void dispatchWork(const QString &op,QJsonObject params,int slot,bool background=false){auto &q=querySlots_[slot];params["_connection_slot"]=slot;params["_connection_epoch"]=double(q.epoch);params["_background"]=background;params["_request_id"]=double(q.requestId=++queryRequestId_);emit work(op,params);}
 void resetQuerySlot(int slot,bool warmAfter=false,bool background=false){
  auto &q=querySlots_[slot];q.cancel->store(true);const auto token=std::make_shared<std::atomic_bool>(true);q.cancel=token;q.epoch=++queryEpoch_;q.requestId=0;q.queryInFlight=false;q.dateKey.clear();q.queryBatch=0;q.ready=false;q.warming=false;q.failed=false;q.warmAttempted=false;q.resetting=true;q.warmAfterReset=warmAfter;q.background=background;q.uses=q.api=q.http=0;q.reportedAgeMs=0;q.observed.invalidate();q.hospital=queryHospital();const quint64 epoch=q.epoch;Worker *worker=queryWorkers_[slot];
  QMetaObject::invokeMethod(worker,[worker,slot,epoch,token]{worker->resetQuery(slot,epoch,token);},Qt::QueuedConnection);
 }
 void resetQueryPool(){++queryPoolGeneration_;for(int slot=0;slot<queryConnectionCount_;++slot)resetQuerySlot(slot);ready_=warming_=busy_=false;warmExpiry_.stop();}
 void beginSlotWarm(int slot){
  auto &q=querySlots_[slot];if(closing_||q.resetting||q.cancel->load()||q.hospital!=queryHospital())return;
  q.warmAfterReset=false;q.warmAttempted=true;q.warming=true;q.ready=false;q.failed=false;q.warmClock.start();q.warmBatch=batchActive_?queryBatchId_:0;
  if(slot==activeQuerySlot_&&!q.background){warming_=true;ready_=false;warmTimer_.start();connection_->setText(QString::fromUtf8("站点连接：正在准备主连接…"));}
  log_->appendPlainText(QString::fromUtf8("连接池槽%1：%2连接准备中；仅预热，不查号。").arg(slot).arg(q.background?QString::fromUtf8("备用"):QString::fromUtf8("主")));
  dispatchWork("prewarm",{{"hospital_code",q.hospital},{"renew_session",true}},slot,q.background);updateControls();
 }
 void queryResetCompleted(int slot,quint64 epoch){
  if(slot<0||slot>=queryConnectionCount_)return;auto &q=querySlots_[slot];if(q.epoch!=epoch)return;q.resetting=false;
  if(closing_)return;q.cancel->store(false);if(q.warmAfterReset&&q.hospital==queryHospital()&&!cancel_->load())beginSlotWarm(slot);else q.warmAfterReset=false;
  mirrorActiveQuerySlot();if(batchActive_&&waitingConnection_)dispatchNextDate();else if(slot==activeQuerySlot_&&pendingLoad_&&!q.warming){const bool force=pendingForce_;pendingLoad_=pendingForce_=false;loadDepartments(force);}updateControls();
 }
 void startSlotWarm(int slot,bool background){
  auto &q=querySlots_[slot];if(closing_||q.warming)return;
  if(q.resetting){q.warmAfterReset=true;q.background=background;q.hospital=queryHospital();if(slot==activeQuerySlot_&&!background)warming_=true;return;}
  resetQuerySlot(slot,true,background);if(slot==activeQuerySlot_&&!background){warming_=true;ready_=false;}updateControls();
 }
 void primeSelectedHospitalPool(){
  if(closing_||hospital_.isEmpty()||batchActive_||busy_||cancel_->load()||!poolRecoveryEnabled_)return;if(poolInitialPrimeDone_){maintainQueryPool();return;}poolInitialPrimeDone_=true;const auto dates=calendar_->selectedDates();const int target=poolReadyTarget();Q_UNUSED(dates);int engaged=0;
  for(int slot=0;slot<queryConnectionCount_;++slot){const auto &q=querySlots_[slot];if(q.hospital==hospital_&&(querySlotReady(slot)||q.warming||(q.resetting&&q.warmAfterReset)))++engaged;}
  for(int slot=0;slot<queryConnectionCount_&&engaged<target;++slot){auto &q=querySlots_[slot];if(q.queryInFlight||q.failed||q.warmAttempted||querySlotReady(slot)||q.warming||(q.resetting&&q.warmAfterReset))continue;startSlotWarm(slot,true);++engaged;}
  if(engaged>0)connection_->setText(QString::fromUtf8("站点连接：后台静默准备 %1 个；可继续选择科室和日期").arg(target));
 }
 void scheduleStandbyPrime(){const quint64 generation=queryPoolGeneration_;QTimer::singleShot(0,this,[this,generation]{if(queryPoolGeneration_==generation)maintainQueryPool();});}
 bool activateReadySpare(){const int spare=readySpareSlot();if(spare<0)return false;querySlots_[activeQuerySlot_].warmAttempted=false;activeQuerySlot_=spare;mirrorActiveQuerySlot();connection_->setText(QString::fromUtf8("站点连接：已切换就绪备用连接；未在前台重新连接"));return true;}
 int inFlightQueryCount()const{int count=0;for(int slot=0;slot<queryConnectionCount_;++slot)if(querySlots_[slot].queryInFlight)++count;return count;}
 void maybePrimeSpare(bool allowIdle=false){
  if(queryConnectionCount_<2||closing_||cancel_->load())return;
  if(allowIdle){maintainQueryPool();return;}
  if(!batchActive_||batchAborting_||!batchPlanReady_||pendingQueryDates_.isEmpty())return;
  // Parallel preparation is limited by unfinished distinct dates, not by a serial reuse estimate.
  const int target=qMin(queryConnectionCount_,pendingQueryDates_.size()+inFlightQueryCount());int engaged=0;
  for(int slot=0;slot<queryConnectionCount_;++slot){const auto &q=querySlots_[slot];if(q.queryInFlight||querySlotReady(slot)||q.warming||(q.resetting&&q.warmAfterReset))++engaged;}
  for(int offset=0;offset<queryConnectionCount_&&engaged<target;++offset){const int slot=(activeQuerySlot_+offset)%queryConnectionCount_;auto &q=querySlots_[slot];if(q.queryInFlight||q.warming||(q.resetting&&q.warmAfterReset)||q.failed||querySlotReady(slot))continue;startSlotWarm(slot,true);++engaged;}

 }
 void rebuildBatchRows(){queryRows_=QJsonArray();QSet<QByteArray> seen;for(const auto &date:queryDates_){for(const auto &value:batchRowsByDate_.value(date.toString("yyyyMMdd"))){const auto key=QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);if(!seen.contains(key)){seen.insert(key);queryRows_.append(value);}}}}
 void cancelOtherQueryWork(int completedSlot){for(int slot=0;slot<queryConnectionCount_;++slot)if(slot!=completedSlot)resetQuerySlot(slot);}
 void cancelPendingBatchWork(){pendingQueryDates_.clear();for(int slot=0;slot<queryConnectionCount_;++slot){auto &q=querySlots_[slot];q.cancel->store(true);q.warmAfterReset=false;}}
 void finishAbortedBatch(){QJsonObject result=batchFailure_;batchActive_=false;batchAborting_=false;waitingConnection_=false;busy_=false;lastQuerySuccess_=false;rebuildBatchRows();result["rows"]=queryRows_;result["queried_date_count"]=queryDateIndex_;result["max_parallel_dates"]=batchMaxInFlight_;result["connection_threads"]=queryConnectionCount_;addBatchPerformance(result);last_=result;repeat_->setChecked(false);repeatTimer_.stop();renderAvailability(queryRows_);status_->setText(QString::fromUtf8("多日期查询中止：已完成 %1 / %2 天；其它任务已取消，失败日期未自动重发。原因：%3").arg(queryDateIndex_).arg(queryDates_.size()).arg(result.value("failure").toString()));performance_->setText(QString::fromUtf8("全批耗时 %1 秒；号源HTTP %2 次；连接HTTP %3 次；合计 %4 次").arg(jobTimer_.elapsed()/1000.0,0,'f',3).arg(batchQueryHttp_).arg(batchConnectionHttp_).arg(batchQueryHttp_+batchConnectionHttp_));resetQueryPool();updateControls();emit resultRendered(result);}

 void observeQuerySlot(int slot,const QJsonObject &result,bool usedDate=false){
  auto &q=querySlots_[slot];const auto p=result.value("performance").toObject();q.reportedAgeMs=qint64(p.value("session_age_seconds").toDouble()*1000.0);q.api=p.value("session_api_used").toInt(q.api);q.http=result.value("runtime").toObject().value("http_dispatches").toInt(q.http);q.observed.restart();if(usedDate)++q.uses;
 }
 void settleCompletedBatchPreparation(QJsonObject &result){
  int pending=0,retained=0,cancelled=0;bool spare=readySpareSlot()>=0;
  for(int slot=0;slot<queryConnectionCount_;++slot){auto &q=querySlots_[slot];if(!(q.warming||(q.resetting&&q.warmAfterReset)))continue;++pending;q.warmBatch=0;
   if(!spare&&retained==0){++retained;continue;}++cancelled;resetQuerySlot(slot);
  }
  result["background_preparations_pending_at_result"]=pending;result["background_preparations_retained"]=retained;result["background_preparations_cancelled"]=cancelled;result["http_totals_complete"]=pending==0;result["http_accounting_phase"]=pending?QStringLiteral("date_results_complete_background_preparation_pending"):QStringLiteral("batch_requests_complete");
 }
 QString selectionPrewarmNote()const{return batchPreparedHttpBeforeClick_>0?QString::fromUtf8("；点击查询前后台预热HTTP %1 次（未计入查询耗时）").arg(batchPreparedHttpBeforeClick_):QString();}
 void addBatchPerformance(QJsonObject &result){auto p=result.value("performance").toObject();p["connection_http_completed_before_query_click"]=batchPreparedHttpBeforeClick_;p["connection_prepare_ms_completed_before_query_click"]=batchPreparedMsBeforeClick_;p["connection_accounting_scope"]="completed_during_query_plus_separate_hospital_selection_prewarm";p["query_http_dispatches_total"]=batchQueryHttp_;p["connection_http_dispatches_total"]=batchConnectionHttp_;p["http_dispatches"]=batchQueryHttp_+batchConnectionHttp_;p["plan_http_dispatches"]=batchPlanHttp_;p["plan_elapsed_ms"]=batchPlanMs_;p["connection_prepare_ms_total"]=batchPrepareMs_;p["max_parallel_dates"]=batchMaxInFlight_;result["performance"]=p;result["query_http_dispatches_total"]=batchQueryHttp_;result["connection_http_dispatches_total"]=batchConnectionHttp_;}
 void queueBudgetReconnect(const QJsonObject &result){Q_UNUSED(result);scheduleStandbyPrime();/* Connection recovery only; failed business requests are never replayed. */}
 bool preparedConnectionReady()const{return !budgetReconnectPending_&&querySlotReady(activeQuerySlot_);}
 void requireManualConnection(){if(!querySlots_[activeQuerySlot_].queryInFlight)querySlots_[activeQuerySlot_].ready=false;ready_=false;warmExpiry_.stop();maintainQueryPool();updateControls();}
 void ensureSelectedConnection(){if(closing_||busy_||warming_||batchActive_)return;if(!preparedConnectionReady())startWarm(true);}
 void observeSession(const QJsonObject &perf){sessionReportedAgeMs_=qint64(perf.value("session_age_seconds").toDouble()*1000.0);sessionApiUsed_=perf.value("session_api_used").toInt();sessionObserved_.restart();}
 void updateControls(){updateBookingControls();bool idle=!busy_&&!closing_&&!batchActive_&&!foregroundPoolResetting();load_->setEnabled(idle&&!warming_&&!hospital_.isEmpty());refresh_->setEnabled(idle&&!warming_&&!hospital_.isEmpty());crawl_->setEnabled(idle&&!warming_&&!poolWarming());crawlWorkers_->setEnabled(idle&&!warming_&&!poolWarming());query_->setEnabled(!busy_&&!closing_&&!batchActive_&&!budgetReconnectPending_&&department_->currentIndex()>0&&!calendar_->selectedDates().isEmpty());stop_->setEnabled(busy_||warming_||batchActive_||debounce_.isActive()||budgetReconnectPending_||poolWarming()||(poolRecoveryEnabled_&&!hospital_.isEmpty()));connectionThreads_->setEnabled(idle&&!poolWarming());updatePoolStatus();hospitals_->setEnabled(idle&&!warming_);department_->setEnabled(idle&&!warming_);date_->setEnabled(idle&&!warming_);calendar_->setEnabled(idle&&!warming_);rows_->setEnabled(!batchActive_);search_->setEnabled(!batchActive_&&!warming_);autoLoad_->setEnabled(!batchActive_);repeat_->setEnabled(!batchActive_);interval_->setEnabled(!batchActive_);}
 void startWarm(bool automatic=false){if(busy_||closing_)return;cancelBudgetReconnect();automaticWarm_=automatic;if(!automatic)budgetReconnectCredit_=true;repeatTimer_.stop();cancel_->store(false);startSlotWarm(activeQuerySlot_,false);}
 void loadDepartments(bool force){cancelBudgetReconnect();clearBookingPreview();Q_UNUSED(force);debounce_.stop();if(hospital_.isEmpty()||busy_||closing_)return;repeatTimer_.stop();lastQuerySuccess_=false;QJsonObject cached=Service::cacheOnly(root_,hospital_);if(!cached.isEmpty()){jobTimer_.start();jobHospital_=hospital_;completed("departments",cached);return;}if(querySlots_[activeQuerySlot_].warming||querySlots_[activeQuerySlot_].resetting){pendingLoad_=true;pendingForce_=force;return;}cancel_->store(false);busy_=true;jobHospital_=hospital_;operation_="departments";if(!selectionTimer_.isValid())selectionTimer_.start();jobTimer_.start();dispatchWork(operation_,{{"hospital_code",hospital_}},activeQuerySlot_);updateControls();}
 void crawlCatalog(){cancelBudgetReconnect();clearBookingPreview();if(busy_||warming_||closing_||poolResetting()||poolWarming())return;querySlots_[0].ready=false;renewalPending_=false;renewalCredit_=false;ready_=false;warmExpiry_.stop();sessionObserved_.invalidate();connection_->setText(QString::fromUtf8("站点连接：科室采集使用独立会话"));debounce_.stop();repeatTimer_.stop();lastQuerySuccess_=false;pendingLoad_=false;pendingForce_=false;cancel_->store(false);busy_=true;operation_="crawl_all_departments";jobTimer_.start();status_->setText(QString::fromUtf8("开始有限并发采集医院清单内的科室；可随时停止，已完成医院会保留断点。"));catalogProgressTimer_.start();dispatchWork(operation_,{{"workers",crawlWorkers_->value()}},0);updateControls();}
  // One distinct date in flight per owner Service; all wire calls keep the shared pacing gate.
  void dispatchNextDate(){
   if(!batchActive_||batchAborting_||closing_||cancel_->load())return;
   if(!batchPlanReady_){
    if(batchPlanInFlight_)return;
    int planSlot=-1;for(int offset=0;offset<queryConnectionCount_;++offset){const int slot=(activeQuerySlot_+offset)%queryConnectionCount_;if(querySlotReady(slot)){planSlot=slot;break;}}
    if(planSlot<0){waitingConnection_=true;int warming=-1;for(int slot=0;slot<queryConnectionCount_;++slot)if(querySlots_[slot].warming||(querySlots_[slot].resetting&&querySlots_[slot].warmAfterReset)){warming=slot;break;}if(warming<0){activeQuerySlot_=0;startSlotWarm(0,false);}const int target=qMin(queryConnectionCount_,queryDates_.size());for(int slot=0;slot<target;++slot){auto &q=querySlots_[slot];if(q.queryInFlight||q.warming||(q.resetting&&q.warmAfterReset)||q.failed||querySlotReady(slot))continue;startSlotWarm(slot,slot!=activeQuerySlot_);}return;}
    auto &q=querySlots_[planSlot];q.queryInFlight=true;q.queryBatch=queryBatchId_;q.dateKey=QStringLiteral("__plan__");batchPlanInFlight_=true;batchPlanTimer_.start();busy_=true;waitingConnection_=false;jobHospital_=hospital_;operation_="availability_plan";QJsonArray dates;for(const auto &date:queryDates_)dates.append(date.toString("yyyyMMdd"));
    dispatchWork(operation_,{{"hospital_code",hospital_},{"department_code",department_->currentData().toString()},{"first_department_code",department_->currentData(Qt::UserRole+1).toString()},{"dates",dates},{"_batch_nonce",batchNonce_},{"require_prepared_session",true},{"pool_reserve_complete_date",true},{"_batch_id",double(queryBatchId_)},{"_date_key",QStringLiteral("__plan__")}},planSlot);status_->setText(QString::fromUtf8("正在读取本批官方日历；无号日期不重复查询医生明细…"));updateControls();return;
   }
   waitingConnection_=false;
   for(int offset=0;offset<queryConnectionCount_&&!pendingQueryDates_.isEmpty();++offset){const int slot=(activeQuerySlot_+offset)%queryConnectionCount_;if(!querySlotReady(slot))continue;
    const QString date=pendingQueryDates_.takeFirst();auto &q=querySlots_[slot];q.queryInFlight=true;q.queryBatch=queryBatchId_;q.dateKey=date;jobHospital_=hospital_;operation_="availability";
    dispatchWork(operation_,{{"hospital_code",hospital_},{"department_code",department_->currentData().toString()},{"first_department_code",department_->currentData(Qt::UserRole+1).toString()},{"date",date},{"require_prepared_session",true},{"pool_reserve_complete_date",batchLegacy_},{"batch_detail_only",!batchLegacy_},{"batch_context",batchContext_},{"_batch_nonce",batchNonce_},{"_batch_id",double(queryBatchId_)},{"_date_key",date}},slot);
   }
   busy_=inFlightQueryCount()>0;batchMaxInFlight_=qMax(batchMaxInFlight_,inFlightQueryCount());
   status_->setText(QString::fromUtf8("已完成 %1 / %2 天；正在查询 %3 天；待查询 %4 天。").arg(queryDateIndex_).arg(queryDates_.size()).arg(inFlightQueryCount()).arg(pendingQueryDates_.size()));
   if(!pendingQueryDates_.isEmpty()){
    waitingConnection_=true;maybePrimeSpare();
    if(!busy_&&!poolWarming()){
     int selected=-1;for(int slot=0;slot<queryConnectionCount_;++slot)if(!querySlots_[slot].failed&&!querySlots_[slot].resetting){selected=slot;break;}
     if(selected>=0){activeQuerySlot_=selected;querySlots_[selected].warmAttempted=false;startSlotWarm(selected,false);}else{batchActive_=false;waitingConnection_=false;status_->setText(QString::fromUtf8("没有可用连接，已停止本批；未重发失败请求。"));}
    }
   }
   updateControls();
  }
  void query(){
   if(batchActive_||busy_||closing_||department_->currentIndex()<=0)return;
   queryDates_=calendar_->selectedDates();if(queryDates_.isEmpty()){status_->setText(QString::fromUtf8("请在月历中至少选择一天。"));return;}
   std::sort(queryDates_.begin(),queryDates_.end());queryDates_.erase(std::unique(queryDates_.begin(),queryDates_.end()),queryDates_.end());pendingQueryDates_.clear();for(const auto &date:queryDates_)pendingQueryDates_.append(date.toString("yyyyMMdd"));finishedQueryDates_.clear();batchRowsByDate_.clear();batchDateRetries_.clear();batchMaxInFlight_=batchPlanHttp_=0;batchPlanMs_=batchPrepareMs_=0;batchPlanReady_=batchPlanInFlight_=batchAborting_=false;batchFailure_=QJsonObject();batchCalendarByDate_.clear();batchContext_=QJsonObject();batchNonce_=QUuid::createUuid().toString(QUuid::WithoutBraces);
   cancelBudgetReconnect();budgetReconnectCredit_=true;clearBookingPreview();noteUserActivity();repeatTimer_.stop();lastQuerySuccess_=false;cancel_->store(false);poolRecoveryEnabled_=true;queryDateIndex_=0;budgetRetryForDate_=0;batchQueryHttp_=batchConnectionHttp_=0;batchLegacy_=legacyQueryHospitals_.contains(queryHospital());for(int slot=0;slot<queryConnectionCount_;++slot){querySlots_[slot].failed=false;if(!querySlotReady(slot)&&!querySlots_[slot].warming)querySlots_[slot].warmAttempted=false;}++queryBatchId_;batchPreparedHttpBeforeClick_=selectionPreparedHttp_;batchPreparedMsBeforeClick_=selectionPreparedMs_;for(int slot=0;slot<queryConnectionCount_;++slot)if(querySlots_[slot].warming||(querySlots_[slot].resetting&&querySlots_[slot].warmAfterReset))querySlots_[slot].warmBatch=queryBatchId_;queryRows_=QJsonArray();queryNotices_.clear();batchActive_=true;waitingConnection_=false;jobTimer_.start();rows_->setRowCount(0);calendar_->clearDoctors();dispatchNextDate();updateControls();
  }
  // Pause sorting while filling cells, so a booking payload never crosses rows.
  void renderAvailability(const QJsonArray &rows){
   const QSignalBlocker blocked(rows_);const bool updates=rows_->updatesEnabled(),sorting=rows_->isSortingEnabled();rows_->setUpdatesEnabled(false);rows_->setSortingEnabled(false);rows_->clearSelection();rows_->setCurrentItem(nullptr);rows_->setRowCount(rows.size());
   for(int r=0;r<rows.size();++r){const auto a=rows[r].toObject();const QStringList values={a.value("date").toString(),a.value("department_name").toString(),a.value("doctor_name").toString(),a.value("period").toString(),(a.value("remaining").isNull()||a.value("remaining").isUndefined())?(a.value("row_kind")==QJsonValue("department_calendar")?QString::fromUtf8("未提供"):a.value("row_kind")==QJsonValue("date_unavailable")?QString::fromUtf8("不适用"):QString::fromUtf8("未知")):a.value("remaining").toVariant().toString(),a.value("status").toString(),queryTimeForDisplay(a.value("queried_at").toString())};for(int c=0;c<values.size();++c)rows_->setItem(r,c,new BookingTableItem(values[c]));rows_->item(r,6)->setToolTip(a.value("queried_at").toString());rows_->item(r,0)->setData(Qt::UserRole,a);
    const auto kind=a.value("row_kind").toString();
    if(kind=="department_calendar"||kind=="date_unavailable"){
     // This is a department/date summary, not a hidden doctor/period result.
     // Missing numeric inventory stays null; never fabricate zero or a doctor.
     const bool calendar=kind=="department_calendar";
     rows_->item(r,2)->setText(QString::fromUtf8("—"));rows_->item(r,3)->setText(calendar?QString::fromUtf8("科室日历"):QString::fromUtf8("日期提示"));
     const auto explanation=calendar?QString::fromUtf8("来源：官方科室日历。官方状态：%1。官方未返回医生明细和剩余数量；“未提供”不表示查询失败，也不能据此判断医生没有出诊。此行不能挂号。").arg(a.value("status").toString()):QString::fromUtf8("该日期不在官方可查询范围；无医生、时段或可挂号标识。此行不能挂号。");
     for(int c=0;c<rows_->columnCount();++c)rows_->item(r,c)->setToolTip(explanation);
    }
   }
   rows_->setSortingEnabled(sorting);rowTools_->refresh();rows_->setUpdatesEnabled(updates);calendar_->setDoctorRows(rows);
  }
 void stop(){poolRecoveryEnabled_=false;batchActive_=false;batchPlanInFlight_=batchAborting_=false;waitingConnection_=false;pendingQueryDates_.clear();queryDates_.clear();cancelBudgetReconnect();budgetReconnectCredit_=false;clearCaptcha(QString::fromUtf8("已停止，请重新获取"));clearBookingPreview();renewalPending_=false;renewalCredit_=false;sessionObserved_.invalidate();debounce_.stop();pendingLoad_=false;pendingForce_=false;cancel_->store(true);resetQueryPool();ready_=false;lastQuerySuccess_=false;warmExpiry_.stop();repeat_->setChecked(false);connection_->setText(QString::fromUtf8("站点连接：已停止/需重新校验"));status_->setText(QString::fromUtf8("正在取消并关闭后台会话…"));updateControls();}
 void completed(QString op,QJsonObject result){
  if(op.startsWith("booking_")){if(result.value("_connection_slot").toInt(0)==0)bookingCompleted(op,result);return;}
  if(closing_)return;
  const bool tagged=result.contains("_connection_slot");const int slot=tagged?result.value("_connection_slot").toInt(-1):activeQuerySlot_;if(slot<0||slot>=queryConnectionCount_)return;auto &q=querySlots_[slot];
  if(tagged&&(quint64(result.value("_connection_epoch").toDouble())!=q.epoch||q.resetting))return;
  const quint64 requestId=quint64(result.value("_request_id").toDouble());if(tagged&&requestId&&requestId!=q.requestId)return;
  if((op=="availability"||op=="availability_plan")&&tagged&&(!batchActive_||!q.queryInFlight||q.queryBatch!=queryBatchId_||quint64(result.value("_batch_id").toDouble())!=queryBatchId_||result.value("_date_key").toString()!=q.dateKey))return;
  if(cancel_->load()&&busy_&&op!="prewarm"){result["status"]="cancelled";result["business_success"]=false;result["failure"]="cancelled_by_user";result["rows"]=QJsonArray();}
  QString status=result.value("status").toString();bool ok=status=="complete"||status=="ready";
  if(op=="prewarm"){
   q.warming=false;q.warmAfterReset=false;q.failed=!ok||cancel_->load();q.ready=ok&&!q.failed&&result.value("session_kept_open").toBool()&&result.value("query_allowed").toBool();observeQuerySlot(slot,result);q.ready=querySlotReady(slot);if(q.ready){q.recoveryFailures=0;q.recoveryAfterMs=0;poolRecoveryFailures_=0;}else notePoolRecoveryFailure(slot,result);
   if(q.background&&q.warmBatch==0){selectionPreparedHttp_+=result.value("runtime").toObject().value("http_dispatches").toInt();if(q.warmClock.isValid())selectionPreparedMs_+=q.warmClock.elapsed();}if(batchActive_&&q.warmBatch==queryBatchId_){batchConnectionHttp_+=result.value("runtime").toObject().value("http_dispatches").toInt();if(q.warmClock.isValid())batchPrepareMs_+=q.warmClock.elapsed();}
   const bool foreground=slot==activeQuerySlot_;if(foreground)mirrorActiveQuerySlot();if(!batchActive_&&q.background&&q.ready)connection_->setText(QString::fromUtf8("站点连接：后台就绪 %1 个；查询将直接复用").arg(readyQueryConnections()));
   const QString text=QString::fromUtf8("%1连接准备%2：%3 秒；连接HTTP %4 次（非号源查询）").arg(q.background?QString::fromUtf8("备用"):QString::fromUtf8("主")).arg(q.ready?QString::fromUtf8("完成"):QString::fromUtf8("失败，等待后台退避恢复")).arg(q.warmClock.isValid()?q.warmClock.elapsed()/1000.0:0,0,'f',3).arg(result.value("runtime").toObject().value("http_dispatches").toInt());QString detail=text;if(!q.ready){detail+=QString::fromUtf8("；原因：%1").arg(result.value("failure").toString(QStringLiteral("connection_not_ready")));const auto transport=result.value("runtime").toObject().value("transport_records").toArray();if(!transport.isEmpty()){const auto last=transport.last().toObject();if(last.contains("curl_code"))detail+=QString::fromUtf8("；网络错误 %1：%2；HTTP %3").arg(last.value("curl_code").toInt()).arg(last.value("transport_error").toString()).arg(last.value("status").toInt());}}log_->appendPlainText(detail);if(foreground&&!q.background&&!batchActive_)performance_->setText(detail);
   if(foreground&&!q.background)connection_->setText(q.ready?QString::fromUtf8("站点连接：主连接就绪"):QString::fromUtf8("站点连接：未就绪，后台退避恢复"));
   if(batchActive_&&batchAborting_){if(inFlightQueryCount()==0&&!poolWarming())finishAbortedBatch();else updateControls();return;}
   if(batchActive_&&waitingConnection_){
    if(batchAborting_){if(inFlightQueryCount()==0&&!poolWarming())finishAbortedBatch();}
    else if(q.ready){dispatchNextDate();}
    else if(!busy_&&!inFlightQueryCount()&&!poolWarming()){batchActive_=false;waitingConnection_=false;queryDates_.clear();repeat_->setChecked(false);status_->setText(QString::fromUtf8("连接准备失败：%1；本批已停止，未自动重试。").arg(result.value("failure").toString()));result["result_kind"]="availability_plan";result["queried_date_count"]=0;addBatchPerformance(result);last_=result;emit resultRendered(result);}
   }
   if(q.ready&&!busy_&&!batchActive_)scheduleStandbyPrime();
   if(foreground&&pendingLoad_){const bool force=pendingForce_;pendingLoad_=pendingForce_=false;if(!cancel_->load())loadDepartments(force);}
   updateControls();return;
  }
  if(op=="availability_plan"){
   q.queryInFlight=false;q.dateKey.clear();batchPlanInFlight_=false;busy_=inFlightQueryCount()>0;const int http=result.value("performance").toObject().value("http_dispatches").toInt();batchQueryHttp_+=http;batchPlanHttp_+=http;batchPlanMs_+=batchPlanTimer_.isValid()?batchPlanTimer_.elapsed():0;observeQuerySlot(slot,result);q.ready=ok&&result.value("session_kept_open").toBool()&&result.value("query_allowed").toBool();q.failed=!ok;
   if(!ok){notePoolRecoveryFailure(slot,result);batchActive_=false;waitingConnection_=false;pendingQueryDates_.clear();cancelOtherQueryWork(slot);busy_=false;addBatchPerformance(result);last_=result;lastQuerySuccess_=false;repeatTimer_.stop();status_->setText(QString::fromUtf8("日历计划失败：%1；本批已停止，未重发请求。").arg(result.value("failure").toString()));performance_->setText(QString::fromUtf8("全批耗时 %1 秒；号源HTTP %2 次；连接HTTP %3 次").arg(jobTimer_.elapsed()/1000.0,0,'f',3).arg(batchQueryHttp_).arg(batchConnectionHttp_));updateControls();emit resultRendered(result);return;}
   batchContext_=result.value("batch_context").toObject();batchPlanReady_=true;batchLegacy_=result.value("hospital_system")==QJsonValue("legacy")||result.value("fallback_required").toBool();if(batchLegacy_)legacyQueryHospitals_.insert(queryHospital());
   batchCalendarByDate_.clear();for(const auto &value:result.value("calendar_rows").toArray()){const auto row=value.toObject();const QString date=row.value("date").toString();if(!date.isEmpty())batchCalendarByDate_[date]=row;}
   pendingQueryDates_.clear();const auto details=result.value("detail_dates").toArray();QSet<QString> selected;for(const auto &date:queryDates_)selected.insert(date.toString("yyyyMMdd"));for(const auto &value:details){const QString date=value.toString();if(selected.contains(date)&&!pendingQueryDates_.contains(date))pendingQueryDates_.append(date);}std::sort(pendingQueryDates_.begin(),pendingQueryDates_.end());
   for(const auto &value:result.value("rows").toArray()){const auto row=value.toObject();const QString date=row.value("date").toString();if(selected.contains(date)){batchRowsByDate_[date].append(row);finishedQueryDates_.insert(date);}}
   for(const auto &date:selected)if(!finishedQueryDates_.contains(date)&&!pendingQueryDates_.contains(date)){batchActive_=false;waitingConnection_=false;pendingQueryDates_.clear();result["status"]="failed";result["business_success"]=false;result["failure"]="selected_date_missing_or_ambiguous";addBatchPerformance(result);last_=result;status_->setText(QString::fromUtf8("官方日历计划缺少所选日期，已停止本批；未推断无号。"));updateControls();emit resultRendered(result);return;}
   queryDateIndex_=finishedQueryDates_.size();rebuildBatchRows();
   if(pendingQueryDates_.isEmpty()){
    batchActive_=false;waitingConnection_=false;busy_=false;result["status"]="complete";result["business_success"]=true;result["rows"]=queryRows_;result["queried_date_count"]=queryDateIndex_;result["max_parallel_dates"]=0;settleCompletedBatchPreparation(result);addBatchPerformance(result);last_=result;lastQuerySuccess_=true;renderAvailability(queryRows_);status_->setText(QString::fromUtf8("实时查询完成：%1 天，%2 行；已用本批官方日历直接返回无需明细的日期。").arg(queryDateIndex_).arg(queryRows_.size()));performance_->setText(QString::fromUtf8("日期结果耗时 %1 秒；号源HTTP %2 次；已完成连接HTTP %3 次；已确认合计 %4 次").arg(jobTimer_.elapsed()/1000.0,0,'f',3).arg(batchQueryHttp_).arg(batchConnectionHttp_).arg(batchQueryHttp_+batchConnectionHttp_));performance_->setText(performance_->text()+QString::fromUtf8("；日历计划 %1 次HTTP / %2 秒；连接准备累计 %3 秒；最大同时查询 0 天").arg(batchPlanHttp_).arg(batchPlanMs_/1000.0,0,'f',3).arg(batchPrepareMs_/1000.0,0,'f',3));performance_->setText(performance_->text()+selectionPrewarmNote());if(repeat_->isChecked())repeatTimer_.start(interval_->value()*1000);mirrorActiveQuerySlot();updateControls();emit resultRendered(result);scheduleStandbyPrime();return;
   }
   renderAvailability(queryRows_);dispatchNextDate();return;
  }
  last_=result;if(op!="availability")busy_=false;
  if(op=="availability"){
   const QString date=q.dateKey.isEmpty()?result.value("_date_key").toString():q.dateKey;q.queryInFlight=false;q.dateKey.clear();const int dateHttp=result.value("performance").toObject().value("http_dispatches").toInt();batchQueryHttp_+=dateHttp;
   if(result.value("hospital_system")==QJsonValue("legacy")){batchLegacy_=true;legacyQueryHospitals_.insert(queryHospital());}
   if(ok&&result.value("calendar_summary_required").toBool()){
    if(batchCalendarByDate_.contains(date)){result["rows"]=QJsonArray{batchCalendarByDate_.value(date)};auto coverage=result.value("coverage").toObject();coverage["calendar_checked"]=true;coverage["selected_date_evidence"]="fresh_batch_calendar";result["coverage"]=coverage;result["batch_calendar_joined"]=true;}
    else if(!result.value("doctor_date_refused").toBool()){ok=false;status="failed";result["status"]="failed";result["business_success"]=false;result["failure"]="selected_date_missing_or_ambiguous";result["rows"]=QJsonArray();}
   }
   observeQuerySlot(slot,result,ok||dateHttp>0);q.ready=ok&&result.value("session_kept_open").toBool()&&result.value("query_allowed").toBool();q.failed=!ok;if(!ok)notePoolRecoveryFailure(slot,result);busy_=inFlightQueryCount()>0;
   if(batchActive_){
    if(batchAborting_){if(!busy_&&!poolWarming())finishAbortedBatch();else updateControls();return;}
    const bool localBudget=result.value("failure")==QJsonValue("public_session_budget_exhausted")&&result.value("session_kept_open")==QJsonValue(true)&&result.value("session_invalidated")==QJsonValue(false);
    if(!ok&&localBudget&&dateHttp==0&&batchDateRetries_.value(date)==0&&!cancel_->load()){batchDateRetries_[date]=1;q.failed=false;q.ready=false;q.warmAttempted=false;pendingQueryDates_.prepend(date);dispatchNextDate();return;}
    if(ok&&jobHospital_==hospital_){
     if(!finishedQueryDates_.contains(date)){finishedQueryDates_.insert(date);QJsonArray rows=result.value("rows").toArray();if(rows.isEmpty()&&result.value("calendar_summary_required").toBool()&&batchCalendarByDate_.contains(date))rows.append(batchCalendarByDate_.value(date));batchRowsByDate_[date]=rows;queryDateIndex_=finishedQueryDates_.size();const auto notice=result.value("message").toString();if(!notice.isEmpty()&&!queryNotices_.contains(notice))queryNotices_.append(notice);}rebuildBatchRows();
     if(!pendingQueryDates_.isEmpty()||busy_){renderAvailability(queryRows_);dispatchNextDate();return;}
     result["rows"]=queryRows_;result["queried_date_count"]=queryDateIndex_;if(!queryNotices_.isEmpty())result["message"]=queryNotices_.join(QString::fromUtf8("\n"));batchActive_=false;waitingConnection_=false;busy_=false;
    }else{batchAborting_=true;batchFailure_=result;waitingConnection_=false;cancelPendingBatchWork();budgetReconnectCredit_=false;cancelBudgetReconnect();if(!busy_&&!poolWarming())finishAbortedBatch();else{status_->setText(QString::fromUtf8("查询失败，正在取消其它日期并汇总已发送HTTP；不会重发失败日期。"));updateControls();}return;}
    result["max_parallel_dates"]=batchMaxInFlight_;result["connection_threads"]=queryConnectionCount_;settleCompletedBatchPreparation(result);addBatchPerformance(result);last_=result;
   }
  }
   if(op=="crawl_all_departments"){catalogProgressTimer_.stop();if(ok){status_->setText(QString::fromUtf8("全院科室快照已完整发布：%1 家医院；新增网络获取 %2 家。").arg(result.value("hospital_count").toInt()).arg(result.value("network_fetched_hospitals").toInt()));performance_->setText(QString::fromUtf8("全量采集耗时 %1 秒；HTTP %2 次；快照：%3").arg(result.value("elapsed_seconds").toDouble(),0,'f',1).arg(result.value("network_requests").toInt()).arg(result.value("snapshot_path").toString()));if(!hospital_.isEmpty())loadDepartments(false);}else status_->setText(QString::fromUtf8("全院采集%1：%2；已完成 %3 / %4 家，进度可续跑。").arg(status).arg(result.value("failure").toString()).arg(result.value("completed_hospitals").toInt()).arg(result.value("hospital_count").toInt()));log_->appendPlainText(status_->text());updateControls();emit resultRendered(result);return;}if(!ok){if(op=="availability"&&(result.value("failure")==QJsonValue("legacy_hospital_api_required")||result.value("hospital_system")==QJsonValue("legacy"))){queryRows_={};renderAvailability({});}if(op=="availability"){renewalPending_=false;renewalCredit_=false;sessionObserved_.invalidate();ready_=false;warmExpiry_.stop();connection_->setText(QString::fromUtf8("站点连接：未就绪；查询失败，会话已关闭"));}lastQuerySuccess_=false;repeatTimer_.stop();const QString failure=result.value("failure").toString();if(failure=="public_session_budget_exhausted"&&result.value("session_kept_open").toBool()){sessionApiUsed_=8;connection_->setText(QString::fromUtf8("站点连接：本地会话额度用尽，将自动恢复"));}status_->setText((failure=="legacy_hospital_api_required"||result.value("hospital_system")==QJsonValue("legacy"))?result.value("message").toString():failure=="public_session_budget_exhausted"?QString::fromUtf8("本会话额度已用尽，本次查询未完成；自动恢复连接后可再查询。"):failure=="manual_connection_required"?QString::fromUtf8("连接不可用；再次点击查询会自动连接。"):failure=="department_snapshot_unavailable"?QString::fromUtf8("本地科室快照尚未覆盖此医院。请先运行一次“全院科室采集”；加载科室不会联网。"):QString::fromUtf8("任务%1：%2").arg(status,failure));QString officialMessage=result.value("official_message").toString();if(officialMessage.isEmpty())for(const auto &v:result.value("responses").toArray()){const auto response=v.toObject();if(response.value("business_code")!=QJsonValue("0000"))officialMessage=response.value("business_message").toString();}if(!officialMessage.isEmpty())status_->setText(status_->text()+QString::fromUtf8("；官方提示：")+officialMessage.left(160));log_->appendPlainText(status_->text());if(op=="availability")queueBudgetReconnect(result);updateControls();emit resultRendered(result);return;}
  if(jobHospital_!=hospital_){updateControls();return;}
  if(op=="departments"){departments_=result.value("departments").toArray();department_->clear();department_->addItem(QString::fromUtf8("请选择单一科室"));for(const auto&v:departments_){auto d=v.toObject();department_->addItem(d.value("first_department_name").toString()+" / "+d.value("name").toString()+" ["+d.value("code").toString()+"]",d.value("code").toString());department_->setItemData(department_->count()-1,d.value("first_department_code").toString(),Qt::UserRole+1);}status_->setText(QString::fromUtf8("科室加载完成：%1 个；来源：%2。").arg(departments_.size()).arg(result.value("metadata_cache_source").toString()));}
  // Batch row replacement to avoid per-cell control updates and intermediate repaints.
   else{directSubmissionLocked_=false;const auto rows=result.value("rows").toArray();QElapsedTimer renderClock;renderClock.start();renderAvailability(rows);result["ui_render_ms"]=double(renderClock.nsecsElapsed())/1e6;status_->setText(QString::fromUtf8("实时查询完成：%1 天，%2 行；号源随时间变化。").arg(result.value("queried_date_count").toInt(1)).arg(rows.size()));int calendarCount=0;for(const auto &value:rows)if(value.toObject().value("row_kind")==QJsonValue("department_calendar"))++calendarCount;
    if(calendarCount>0)status_->setText(status_->text()+QString::fromUtf8("\n其中 %1 行为官方科室日历状态；官方未返回对应医生明细和余量数字。“无号”为官方当前状态，数量未提供不显示为0。").arg(calendarCount));
    const auto notice=result.value("message").toString();if(!notice.isEmpty())status_->setText(status_->text()+QString::fromUtf8("\n")+notice);}
  auto perf=result.value("performance").toObject();if(op=="availability"){mirrorActiveQuerySlot();lastQuerySuccess_=true;if(ready_)connection_->setText(QString::fromUtf8("站点连接：就绪；后续查询复用连接"));else connection_->setText(QString::fromUtf8("站点连接：待下次查询切换或恢复"));if(repeat_->isChecked())repeatTimer_.start(interval_->value()*1000);}
  double ms=jobTimer_.isValid()?jobTimer_.elapsed():0;if(op=="departments"&&selectionTimer_.isValid()){ms=selectionTimer_.elapsed();selectionTimer_.invalidate();}
  performance_->setText(op=="availability"?QString::fromUtf8("日期结果耗时 %1 秒；号源HTTP %2 次；已完成连接HTTP %3 次；已确认合计 %4 次").arg(ms/1000.0,0,'f',3).arg(perf.value("query_http_dispatches_total").toInt(perf.value("http_dispatches").toInt())).arg(perf.value("connection_http_dispatches_total").toInt()).arg(perf.value("http_dispatches").toInt()):QString::fromUtf8("界面总耗时 %1 秒；HTTP %2 次；科室缓存 %3").arg(ms/1000.0,0,'f',3).arg(perf.value("http_dispatches").toInt()).arg(perf.value("metadata_cache_hit").toBool()?QString::fromUtf8("命中"):QString::fromUtf8("未命中")));
  if(op=="availability")performance_->setText(performance_->text()+QString::fromUtf8("；日历计划 %1 次HTTP / %2 秒；连接准备累计 %3 秒；最大同时查询 %4 天").arg(batchPlanHttp_).arg(batchPlanMs_/1000.0,0,'f',3).arg(batchPrepareMs_/1000.0,0,'f',3).arg(batchMaxInFlight_));if(op=="availability")performance_->setText(performance_->text()+selectionPrewarmNote());result["connection_phase"]=op=="availability"?QStringLiteral("calendar_plan_parallel_detail_query"):QStringLiteral("local_metadata");result["ui_elapsed_ms"]=ms;result["department_count"]=departments_.size();log_->appendPlainText(status_->text()+" "+performance_->text());last_=result;updateControls();emit resultRendered(result);if(op=="availability"&&ok)scheduleStandbyPrime();
 }
protected:void closeEvent(QCloseEvent *event)override{poolRecoveryEnabled_=false;poolRecoveryTimer_.stop();batchActive_=false;waitingConnection_=false;cancelBudgetReconnect();budgetReconnectCredit_=false;if(queryOwnersRunning()){if(!closing_){closing_=true;renewalPending_=false;renewalCredit_=false;cancel_->store(true);for(auto &slot:querySlots_){slot.cancel->store(true);slot.ready=false;slot.warmAfterReset=false;}debounce_.stop();repeatTimer_.stop();warmExpiry_.stop();captchaExpiry_.stop();emit shutdown();status_->setText(QString::fromUtf8("正在清理全部后台连接…"));}event->ignore();}else event->accept();}
};
static QString arg(const QStringList&a,const QString&key,const QString&def=QString()){int i=a.indexOf(key);return i>=0&&i+1<a.size()?a[i+1]:def;}
static QString defaultProjectRoot(){
 QDir dir(QCoreApplication::applicationDirPath());
 for(int level=0;level<7;++level){if(QFileInfo(dir.filePath("data/hospitals.csv")).isFile())return dir.absolutePath();if(!dir.cdUp())break;}
 return QCoreApplication::applicationDirPath();
}

int main(int argc,char**argv){
 QElapsedTimer startup;startup.start();QApplication app(argc,argv);app.setApplicationName(QString::fromUtf8("挂号软件"));auto args=app.arguments();QString root=QDir(arg(args,"--project-root",defaultProjectRoot())).absolutePath();QString output=arg(args,"--output");
 if(args.contains("--booking-self-test")){auto result=BookingService::offlineSelfTest();return writeJson(output,result)&&result.value("status").toString()=="PASS"?0:1;}
 if(args.contains("--self-test")){auto result=Service::offlineSelfTest();return writeJson(output,result)&&result.value("status").toString()=="PASS"?0:1;}
 if(args.contains("--cli")){std::atomic_bool cancelled(false);QString op=arg(args,"--operation","departments");if(op=="crawl_all_departments"){const auto result=Service::crawlAllDepartments(root,&cancelled,arg(args,"--crawl-workers","4").toInt());if(output.isEmpty())return 2;if(!writeJson(output,result))return 3;return result.value("status").toString()=="complete"?0:1;}Service service(root);QJsonObject params{{"hospital_code",arg(args,"--hospital","H02110003B")},{"department_code",arg(args,"--department")},{"first_department_code",arg(args,"--first-department")},{"date",arg(args,"--date",QDate::currentDate().toString("yyyyMMdd"))},{"force_metadata_refresh",args.contains("--force")}};if(op=="availability_plan"&&args.contains("--dates")){QJsonArray dates;for(const auto &date:arg(args,"--dates").split(",",Qt::SkipEmptyParts))dates.append(date.trimmed());params["dates"]=dates;}QJsonObject result;try{if(args.contains("--prewarm-first")){QElapsedTimer t;t.start();auto warm=service.execute("prewarm",params,&cancelled);warm["external_wall_ms"]=double(t.nsecsElapsed())/1e6;if(!output.isEmpty())writeJson(output+".prewarm.json",warm);if(warm.value("status").toString()!="ready")throw Failure("prewarm_failed");}QElapsedTimer t;t.start();int n=std::max(1,arg(args,"--benchmark-service","1").toInt());QJsonArray durations;for(int i=0;i<n;++i){QElapsedTimer call;call.start();result=service.execute(op,params,&cancelled);durations.append(double(call.nsecsElapsed())/1e6);}result["external_wall_ms"]=double(t.nsecsElapsed())/1e6;result["call_durations_ms"]=durations;result["implementation"]="C++17 / Qt5 / libcurl; Node.js signing; no Python runtime";service.close();result["cleanup_complete"]=true;}catch(const std::exception&e){service.close();result={{"status","failed"},{"failure",QString::fromUtf8(e.what())},{"cleanup_complete",true}};}if(output.isEmpty())return 2;if(!writeJson(output,result))return 3;return result.value("status").toString()=="complete"||result.value("status").toString()=="ready"?0:1;}
 Window window(root,!args.contains("--no-prewarm"));window.show();
 if(args.contains("--benchmark-selection-query")){
  QList<QDate> dates;for(const auto &text:arg(args,"--dates").split(",",Qt::SkipEmptyParts)){const QDate date=QDate::fromString(text.trimmed(),"yyyyMMdd");if(!date.isValid()){writeJson(output,{{"status","failed"},{"failure","benchmark_dates_invalid"}});return 2;}dates.append(date);}const int threads=arg(args,"--query-threads","7").toInt();QElapsedTimer selected,clicked;bool querying=false;QJsonObject preparation;QTimer poll;
  QObject::connect(&window,&Window::resultRendered,&window,[&](QJsonObject result){if(!querying||result.value("result_kind")==QJsonValue("departments"))return;result["benchmark_query_click_to_result_ms"]=double(clicked.nsecsElapsed())/1e6;result["benchmark_hospital_click_to_result_ms"]=double(selected.nsecsElapsed())/1e6;result["benchmark_hospital_selection_preparation"]=preparation;result["benchmark_real_network"]=true;result["benchmark_requested_days"]=dates.size();result["benchmark_query_target_met"]=result.value("status")==QJsonValue("complete")&&result.value("queried_date_count").toInt()==dates.size()&&result.value("benchmark_query_click_to_result_ms").toDouble()<=1000;writeJson(output,result);window.close();});
  QObject::connect(&poll,&QTimer::timeout,&window,[&]{const auto state=window.selectionPrewarmState();if(state.value("ready").toInt()>=state.value("target").toInt()){poll.stop();preparation=state;preparation["hospital_click_to_ready_ms"]=double(selected.nsecsElapsed())/1e6;querying=true;clicked.start();if(!window.benchmarkQuery(arg(args,"--hospital"),arg(args,"--department"),arg(args,"--first-department"),dates,threads)){writeJson(output,{{"status","failed"},{"failure","benchmark_selection_unavailable"},{"preparation",preparation}});window.close();}}else if(selected.elapsed()>15000||(!state.value("resetting").toBool()&&state.value("warming").toInt()==0)){poll.stop();writeJson(output,{{"status","failed"},{"failure","selection_prewarm_not_ready"},{"preparation",state},{"hospital_click_elapsed_ms",double(selected.nsecsElapsed())/1e6}});window.close();}});
  QTimer::singleShot(0,&window,[&]{selected.start();if(!window.benchmarkPrimeHospital(arg(args,"--hospital"),dates,threads)){writeJson(output,{{"status","failed"},{"failure","benchmark_hospital_unavailable"}});window.close();return;}poll.start(20);});QTimer::singleShot(45000,&window,[&]{poll.stop();writeJson(output,{{"status","failed"},{"failure","benchmark_wall_timeout"}});window.close();});return app.exec();
 }
 if(args.contains("--benchmark-query")){
  QList<QDate> dates;for(const auto &text:arg(args,"--dates").split(",",Qt::SkipEmptyParts)){const QDate date=QDate::fromString(text.trimmed(),"yyyyMMdd");if(!date.isValid()){writeJson(output,{{"status","failed"},{"failure","benchmark_dates_invalid"}});return 2;}dates.append(date);}const int threads=arg(args,"--query-threads","7").toInt(),repeats=qBound(1,arg(args,"--query-repeats","1").toInt(),3);QJsonArray runs;QElapsedTimer wall;bool started=false;int completedRuns=0;
  QObject::connect(&window,&Window::resultRendered,&window,[&](QJsonObject result){const auto kind=result.value("result_kind").toString();if(!started||kind=="departments")return;result["benchmark_click_to_result_ms"]=double(wall.nsecsElapsed())/1e6;result["benchmark_run"]=completedRuns+1;result["benchmark_connection_state"]=completedRuns==0?"cold_process":"retained_connections";result["benchmark_requested_days"]=dates.size();result["benchmark_configured_threads"]=threads;result["benchmark_target_ms"]=1000;result["benchmark_target_met"]=result.value("status")==QJsonValue("complete")&&result.value("queried_date_count").toInt()==dates.size()&&result.value("benchmark_click_to_result_ms").toDouble()<=1000;runs.append(result);++completedRuns;if(result.value("status")!=QJsonValue("complete")||completedRuns>=repeats){writeJson(output,{{"status",result.value("status")},{"runs",runs},{"real_network",true},{"automatic_failed_request_retry",false}});window.close();return;}QTimer::singleShot(0,&window,[&]{wall.restart();if(!window.benchmarkQuery(arg(args,"--hospital"),arg(args,"--department"),arg(args,"--first-department"),dates,threads)){writeJson(output,{{"status","failed"},{"failure","benchmark_selection_unavailable"},{"runs",runs}});window.close();}});});
  QTimer::singleShot(0,&window,[&]{started=true;wall.start();if(!window.benchmarkQuery(arg(args,"--hospital"),arg(args,"--department"),arg(args,"--first-department"),dates,threads)){writeJson(output,{{"status","failed"},{"failure","benchmark_selection_unavailable"},{"runs",runs}});window.close();}});QTimer::singleShot(45000,&window,[&]{if(!window.isVisible())return;writeJson(output,{{"status","failed"},{"failure","benchmark_wall_timeout"},{"runs",runs},{"elapsed_ms",double(wall.nsecsElapsed())/1e6}});window.close();});
  return app.exec();
 }
 if(args.contains("--benchmark-startup")&&!args.contains("--benchmark-cache")){QTimer::singleShot(0,&window,[&]{QJsonObject result{{"qt_visible",window.isVisible()},{"internal_elapsed_ms",double(startup.nsecsElapsed())/1e6},{"process_id",double(QCoreApplication::applicationPid())},{"implementation","C++ / Qt5"}};const auto bookingUi=window.bookingUiState();for(auto it=bookingUi.begin();it!=bookingUi.end();++it)result.insert(it.key(),it.value());writeJson(output,result);window.close();});}
 if(args.contains("--benchmark-cached-gui")||args.contains("--benchmark-cache")){QObject::connect(&window,&Window::resultRendered,&window,[&](QJsonObject result){result["qt_visible"]=window.isVisible();result["internal_elapsed_ms"]=double(startup.nsecsElapsed())/1e6;result["cache_click_to_render_ms"]=result.value("ui_elapsed_ms");writeJson(output,result);window.close();});QTimer::singleShot(0,&window,[&]{window.benchmarkSelect(arg(args,"--hospital","H02"));});}return app.exec();
}
#include "main.moc"
