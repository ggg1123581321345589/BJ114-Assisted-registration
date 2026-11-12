#include "native_service.h"
#include "booking_service.h"
#include "booking_calendar.h"
#include "booking_table_tools.h"
#include <QtWidgets>
#include <QJsonDocument>
#include <QSaveFile>
#include <QThread>
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
 QString root_;std::unique_ptr<Service> service_;std::unique_ptr<BookingService> booking_;std::shared_ptr<std::atomic_bool> cancel_;
public:Worker(QString root,std::shared_ptr<std::atomic_bool> cancel):root_(std::move(root)),cancel_(std::move(cancel)){}
public slots:
 void execute(QString op,QJsonObject params){QJsonObject result;try{if(op.startsWith("booking_")){if(!booking_)booking_=std::make_unique<BookingService>(root_);result=booking_->execute(op,params,cancel_.get());}else if(op=="crawl_all_departments"){if(service_){service_->close();service_.reset();}result=Service::crawlAllDepartments(root_,cancel_.get(),params.value("workers").toInt(4));}else{if(!service_)service_=std::make_unique<Service>(root_);result=service_->execute(op,params,cancel_.get());}}catch(const std::exception&e){result={{"status",cancel_->load()?"cancelled":"failed"},{"failure",QString::fromUtf8(e.what())},{"message",QString::fromUtf8("任务未完成，请检查输入或稍后重新操作。")},{"rows",QJsonArray()},{"departments",QJsonArray()}};}emit completed(op,result);}
 void shutdown(){if(service_)service_->close();service_.reset();if(booking_)booking_->close();booking_.reset();emit stopped();}
signals:void completed(QString operation,QJsonObject result);void stopped();
};
class Window final:public QMainWindow {
 friend class UiProbe;
 friend class BookingUiProbe;
 Q_OBJECT
 QString root_,hospital_,jobHospital_;QJsonArray departments_;QJsonObject last_;
 QThread thread_;Worker *worker_;std::shared_ptr<std::atomic_bool> cancel_=std::make_shared<std::atomic_bool>(false);
 QLineEdit *search_;QTableWidget *hospitals_,*rows_;QComboBox *department_;QDateEdit *date_; // Hidden compatibility anchor; users select dates on calendar_.
  BookingCalendar *calendar_;BookingTableTools *hospitalTools_,*rowTools_;
  QList<QDate> queryDates_;QStringList queryNotices_;QJsonArray queryRows_;int queryDateIndex_=0,budgetRetryForDate_=0,sessionHttpUsed_=0;bool batchActive_=false,waitingConnection_=false;
 QPushButton *load_,*refresh_,*crawl_,*query_,*stop_,*prepareConnection_;QCheckBox *autoLoad_,*repeat_;QSpinBox *interval_,*crawlWorkers_;QLabel *connection_,*status_,*performance_;QPlainTextEdit *log_;
 QTimer debounce_,repeatTimer_,warmExpiry_,elapsedTimer_,catalogProgressTimer_,budgetReconnectTimer_;bool budgetReconnectCredit_=false,budgetReconnectPending_=false,automaticWarm_=false;QElapsedTimer jobTimer_,selectionTimer_,warmTimer_,sessionObserved_,userActivity_;qint64 sessionReportedAgeMs_=0;int sessionApiUsed_=0;bool renewalPending_=false,renewalCredit_=true;
 bool busy_=false,warming_=false,ready_=false,closing_=false,pendingLoad_=false,pendingForce_=false,lastQuerySuccess_=false;QString operation_;
 QLineEdit *phone_,*imageCode_,*smsCode_;QLabel *captchaImage_,*loginState_,*bookingSummary_;
 QPushButton *getCaptcha_,*sendSms_,*login_,*logout_,*loadPatients_,*previewBooking_,*submitBooking_;
 QComboBox *patient_,*card_;QCheckBox *terms_;QTimer smsTimer_,captchaExpiry_;int smsSeconds_=0;bool captchaLoaded_=false;
 bool loggedIn_=false,directSubmissionLocked_=false;QString bookingPreviewId_;QJsonObject bookingPreview_;QJsonArray bookingPatients_;
public:
 Window(QString root,bool warm):root_(std::move(root)){userActivity_.start();
  setWindowTitle(QString::fromUtf8("辅助挂号软件 v1.1.0"));resize(1250,1000);
  auto *central=new QWidget;auto *scroll=new QScrollArea;scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);scroll->setWidget(central);setCentralWidget(scroll);auto *layout=new QVBoxLayout(central);
  auto *title=new QLabel(QString::fromUtf8("免责协议"));title->setStyleSheet("font-size:16px;font-weight:bold;");layout->addWidget(title);auto *disclaimer=new QLabel(QString::fromUtf8("本软件为非官方辅助工具，供用户本人查询号源并在人工确认后提交一次预约，不保证号源可用或挂号成功。请遵守医院及官方平台规则，核对就诊人、就诊卡、日期、费用及取退号要求；预约和支付结果以官方平台为准。验证码须由本人输入，遇到提交结果未知时请先核对官方订单，勿重复提交。请妥善保护个人信息，勿用于倒号、代抢或其他违规用途。"));disclaimer->setObjectName("disclaimerAgreement");disclaimer->setWordWrap(true);disclaimer->setTextFormat(Qt::PlainText);layout->addWidget(disclaimer);
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
  auto *warmRow=new QHBoxLayout;prepareConnection_=nullptr;connection_=new QLabel(QString::fromUtf8("站点连接：自动连接已启用"));connection_->setObjectName("automaticConnectionStatus");warmRow->addWidget(connection_,1);layout->addLayout(warmRow);
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
   worker_=new Worker(root_,cancel_);worker_->moveToThread(&thread_);connect(this,&Window::work,worker_,&Worker::execute);connect(this,&Window::shutdown,worker_,&Worker::shutdown);connect(worker_,&Worker::completed,this,&Window::completed);connect(worker_,&Worker::stopped,&thread_,&QThread::quit,Qt::DirectConnection);connect(&thread_,&QThread::finished,worker_,&QObject::deleteLater);connect(&thread_,&QThread::finished,this,[this]{if(closing_)close();});thread_.start();
  debounce_.setSingleShot(true);connect(&debounce_,&QTimer::timeout,this,[this]{loadDepartments(false);});warmExpiry_.setSingleShot(true);connect(&warmExpiry_,&QTimer::timeout,this,[this]{requireManualConnection();});catalogProgressTimer_.setInterval(1000);connect(&catalogProgressTimer_,&QTimer::timeout,this,[this]{QDir base(QDir(root_).filePath("output/hospital_availability_http_fast_gui/department_snapshot_staging"));const auto dirs=base.entryInfoList(QDir::Dirs|QDir::NoDotAndDotDot,QDir::Time);if(dirs.isEmpty())return;QFile f(QDir(dirs.first().absoluteFilePath()).filePath("progress.json"));if(!f.open(QIODevice::ReadOnly))return;const QByteArray progressBytes=f.readAll();f.close();QJsonParseError e;auto d=QJsonDocument::fromJson(progressBytes,&e);if(e.error!=QJsonParseError::NoError||!d.isObject())return;const auto p=d.object();status_->setText(QString::fromUtf8("全院科室采集：%1 / %2 家完成；当前 %3；进度已逐院保存。").arg(p.value("completed_count").toInt()).arg(p.value("hospital_count").toInt()).arg(p.value("current_hospital_code").toString()));});
  budgetReconnectTimer_.setSingleShot(true);connect(&budgetReconnectTimer_,&QTimer::timeout,this,[this]{const bool pending=budgetReconnectPending_;budgetReconnectPending_=false;if(pending&&!busy_&&!warming_&&!closing_&&!cancel_->load())startWarm(true);});repeatTimer_.setSingleShot(true);connect(&repeatTimer_,&QTimer::timeout,this,[this]{if(lastQuerySuccess_&&!busy_&&!warming_)query();});connect(repeat_,&QCheckBox::toggled,this,[this](bool yes){repeatTimer_.stop();if(yes&&lastQuerySuccess_)repeatTimer_.start(interval_->value()*1000);});connect(interval_,qOverload<int>(&QSpinBox::valueChanged),this,[this](int n){repeatTimer_.stop();if(repeat_->isChecked()&&lastQuerySuccess_)repeatTimer_.start(n*1000);});
   connect(search_,&QLineEdit::textChanged,this,[this](const QString&text){hospitalTools_->globalSearch(text);});
  connect(hospitals_,&QTableWidget::itemSelectionChanged,this,[this]{int r=hospitals_->currentRow();if(r<0){hospital_.clear();departments_=QJsonArray();department_->clear();department_->addItem(QString::fromUtf8("请选择医院和科室"));calendar_->clearDoctors();rows_->setRowCount(0);updateControls();return;}cancelBudgetReconnect();clearBookingPreview();repeatTimer_.stop();lastQuerySuccess_=false;pendingLoad_=false;pendingForce_=false;noteUserActivity();hospital_=hospitals_->item(r,1)->text();departments_=QJsonArray();department_->clear();department_->addItem(QString::fromUtf8("请选择科室"));calendar_->clearDoctors();rows_->setRowCount(0);selectionTimer_.start();updateControls();if(autoLoad_->isChecked())debounce_.start(100);});
  connect(load_,&QPushButton::clicked,this,[this]{loadDepartments(false);});connect(refresh_,&QPushButton::clicked,this,[this]{loadDepartments(true);});connect(crawl_,&QPushButton::clicked,this,&Window::crawlCatalog);connect(query_,&QPushButton::clicked,this,[this]{budgetReconnectCredit_=true;query();});connect(stop_,&QPushButton::clicked,this,&Window::stop);connect(department_,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{calendar_->clearDoctors();cancelBudgetReconnect();clearBookingPreview();if(department_->currentIndex()>0){noteUserActivity();ensureSelectedConnection();}repeatTimer_.stop();lastQuerySuccess_=false;rows_->setRowCount(0);updateControls();});calendar_->selectionChanged=[this]{cancelBudgetReconnect();clearBookingPreview();const auto dates=calendar_->selectedDates();if(!dates.isEmpty()){const QSignalBlocker blocked(date_);date_->setDate(dates.first());}noteUserActivity();repeatTimer_.stop();lastQuerySuccess_=false;rows_->setRowCount(0);updateControls();};
   connect(date_,&QDateEdit::dateChanged,this,[this](const QDate &date){calendar_->setSelectedDates({date});});
  connect(&elapsedTimer_,&QTimer::timeout,this,[this]{const auto &clock=warming_?warmTimer_:jobTimer_;if(clock.isValid()&&(busy_||warming_))performance_->setText((warming_?QString::fromUtf8("自动连接已等待 %1 秒（尚未发起号源查询）"):QString::fromUtf8("任务已等待 %1 秒（含队列、网络和界面处理）")).arg(clock.elapsed()/1000.0,0,'f',3));});elapsedTimer_.start(100);updateControls();if(warm)QTimer::singleShot(0,this,[this]{if(!busy_&&!warming_&&!closing_&&!cancel_->load())startWarm(true);});
 }
 ~Window()override{cancel_->store(true);if(thread_.isRunning()){emit shutdown();thread_.quit();thread_.wait(30000);}}
 QJsonObject bookingUiState()const{return {{"booking_enabled",true},{"login_controls_present",bool(phone_&&imageCode_&&smsCode_&&login_&&sendSms_)},{"patient_controls_present",bool(patient_&&card_&&previewBooking_&&submitBooking_)},{"manual_connection_preparation",bool(prepareConnection_)},{"automatic_connection_renewal",true},{"automatic_connection_renewal_scope","startup_selection_query_and_local_budget"},{"multi_date_calendar",true},{"calendar_doctor_annotations",true},{"table_header_sort_and_filter",true},{"manual_submit_confirmation",true},{"app_title",windowTitle()}};}
 void benchmarkSelect(const QString &code){for(int r=0;r<hospitals_->rowCount();++r)if(hospitals_->item(r,1)->text()==code){hospitals_->setCurrentCell(r,0);debounce_.stop();loadDepartments(false);return;}}
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
  if(!getCaptcha_)return;const bool idle=!busy_&&!warming_&&!closing_&&!batchActive_;
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
  if(busy_||warming_||closing_||batchActive_)return;cancelBudgetReconnect();if((op=="booking_sms"||op=="booking_login")&&!terms_->isChecked()){loginState_->setText(QString::fromUtf8("请先阅读并同意官方用户协议和隐私政策。"));return;}if(op=="booking_sms"&&!captchaLoaded_){loginState_->setText(QString::fromUtf8("请先获取有效图形验证码。"));return;}if(op=="booking_captcha"){clearCaptcha(QString::fromUtf8("正在加载图形码…"));getCaptcha_->setText(QString::fromUtf8("正在加载…"));}else if(op=="booking_sms")clearCaptcha(QString::fromUtf8("本次图形码已使用"));if(op!="booking_submit")clearBookingPreview();repeatTimer_.stop();debounce_.stop();repeat_->setChecked(false);cancel_->store(false);busy_=true;operation_=op;jobTimer_.start();
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
  // Refresh once for an exhausted local session; never replay a query or a booking.
  void queueBudgetReconnect(const QJsonObject &result){
   if(!budgetReconnectCredit_||budgetReconnectPending_||busy_||warming_||closing_||cancel_->load()||hospital_.isEmpty()||jobHospital_!=hospital_||result.value("hospital_code").toString()!=hospital_)return;
   const bool retained=result.value("session_kept_open")==QJsonValue(true);
   const bool localBudget=result.value("failure")==QJsonValue("public_session_budget_exhausted")&&result.value("session_invalidated")==QJsonValue(false);
   const bool complete=result.value("status")==QJsonValue("complete")&&result.value("business_success")==QJsonValue(true);
   const auto perf=result.value("performance").toObject();const auto runtime=result.value("runtime").toObject();
   const bool exhausted=perf.value("session_api_used").toInt(-1)>=8||runtime.value("http_dispatches").toInt(-1)>=12;
   if(!retained||result.value("session_invalidated")==QJsonValue(true)||(!localBudget&&!complete)||!exhausted)return;
   budgetReconnectCredit_=false;budgetReconnectPending_=true;ready_=false;warmExpiry_.stop();repeatTimer_.stop();
   connection_->setText(QString::fromUtf8("站点连接：本地会话额度用尽，1秒后自动准备连接；不会重发查询"));budgetReconnectTimer_.start(1000);updateControls();
  }
   bool preparedConnectionReady()const{return !budgetReconnectPending_&&ready_&&sessionObserved_.isValid()&&sessionAgeMs()<100000&&sessionApiUsed_<8&&sessionHttpUsed_<12;}
   // Expiry marks the session stale; the next user query reconnects automatically.
   void requireManualConnection(){ready_=false;warmExpiry_.stop();connection_->setText(QString::fromUtf8("站点连接：待自动连接；查询时自动恢复"));updateControls();}
   void ensureSelectedConnection(){if(closing_||busy_||warming_||batchActive_)return;if(!preparedConnectionReady())startWarm(true);}
 void observeSession(const QJsonObject &perf){sessionReportedAgeMs_=qint64(perf.value("session_age_seconds").toDouble()*1000.0);sessionApiUsed_=perf.value("session_api_used").toInt();sessionObserved_.restart();}
  void updateControls(){updateBookingControls();bool idle=!busy_&&!closing_&&!batchActive_;load_->setEnabled(idle&&!warming_&&!hospital_.isEmpty());refresh_->setEnabled(idle&&!warming_&&!hospital_.isEmpty());crawl_->setEnabled(idle&&!warming_);crawlWorkers_->setEnabled(idle&&!warming_);query_->setEnabled(idle&&!warming_&&!budgetReconnectPending_&&department_->currentIndex()>0&&!calendar_->selectedDates().isEmpty());stop_->setEnabled(busy_||warming_||batchActive_||debounce_.isActive()||budgetReconnectPending_);hospitals_->setEnabled(idle&&!warming_);department_->setEnabled(idle&&!warming_);date_->setEnabled(idle&&!warming_);calendar_->setEnabled(idle&&!warming_);rows_->setEnabled(!batchActive_);search_->setEnabled(!batchActive_&&!warming_);autoLoad_->setEnabled(!batchActive_);repeat_->setEnabled(!batchActive_);interval_->setEnabled(!batchActive_);}
  // Prepare manually, or once after verified local budget exhaustion; never replay queries.
  void startWarm(bool automatic=false){if(warming_||busy_||closing_)return;cancelBudgetReconnect();automaticWarm_=automatic;if(!automatic)budgetReconnectCredit_=true;warmExpiry_.stop();repeatTimer_.stop();warming_=true;ready_=false;cancel_->store(false);warmTimer_.start();connection_->setText(QString::fromUtf8("站点连接：正在自动连接…"));emit work("prewarm",{{"hospital_code",hospital_.isEmpty()?QStringLiteral("H01110005"):hospital_},{"renew_session",true}});updateControls();}
 void loadDepartments(bool force){cancelBudgetReconnect();clearBookingPreview();Q_UNUSED(force);debounce_.stop();if(hospital_.isEmpty()||busy_||closing_)return;repeatTimer_.stop();lastQuerySuccess_=false;QJsonObject cached=Service::cacheOnly(root_,hospital_);if(!cached.isEmpty()){jobTimer_.start();jobHospital_=hospital_;completed("departments",cached);return;}cancel_->store(false);busy_=true;jobHospital_=hospital_;operation_="departments";if(!selectionTimer_.isValid())selectionTimer_.start();jobTimer_.start();emit work(operation_,{{"hospital_code",hospital_}});updateControls();}
 void crawlCatalog(){cancelBudgetReconnect();clearBookingPreview();if(busy_||warming_||closing_)return;renewalPending_=false;renewalCredit_=false;ready_=false;warmExpiry_.stop();sessionObserved_.invalidate();connection_->setText(QString::fromUtf8("站点连接：科室采集使用独立会话"));debounce_.stop();repeatTimer_.stop();lastQuerySuccess_=false;pendingLoad_=false;pendingForce_=false;cancel_->store(false);busy_=true;operation_="crawl_all_departments";jobTimer_.start();status_->setText(QString::fromUtf8("开始有限并发采集医院清单内的科室；可随时停止，已完成医院会保留断点。"));catalogProgressTimer_.start();emit work(operation_,{{"workers",crawlWorkers_->value()}});updateControls();}
  // One finite batch, one in-flight date. Existing service pacing stays in force.
  void dispatchNextDate(){
   if(!batchActive_||busy_||warming_||closing_||cancel_->load()||queryDateIndex_>=queryDates_.size())return;
   if(!preparedConnectionReady()){waitingConnection_=true;startWarm(true);return;}
   waitingConnection_=false;busy_=true;jobHospital_=hospital_;operation_="availability";
   status_->setText(QString::fromUtf8("正在查询 %1（%2 / %3 天）…").arg(queryDates_[queryDateIndex_].toString("yyyy-MM-dd")).arg(queryDateIndex_+1).arg(queryDates_.size()));
   emit work(operation_,{{"hospital_code",hospital_},{"department_code",department_->currentData().toString()},{"first_department_code",department_->currentData(Qt::UserRole+1).toString()},{"date",queryDates_[queryDateIndex_].toString("yyyyMMdd")},{"require_prepared_session",true}});updateControls();
  }
  void query(){
   if(batchActive_||busy_||warming_||closing_||department_->currentIndex()<=0)return;
   queryDates_=calendar_->selectedDates();if(queryDates_.isEmpty()){status_->setText(QString::fromUtf8("请在月历中至少选择一天。"));return;}
   cancelBudgetReconnect();budgetReconnectCredit_=true;clearBookingPreview();noteUserActivity();repeatTimer_.stop();lastQuerySuccess_=false;cancel_->store(false);queryDateIndex_=0;budgetRetryForDate_=0;queryRows_=QJsonArray();queryNotices_.clear();batchActive_=true;waitingConnection_=false;jobTimer_.start();rows_->setRowCount(0);calendar_->clearDoctors();dispatchNextDate();updateControls();
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
 void stop(){batchActive_=false;waitingConnection_=false;queryDates_.clear();cancelBudgetReconnect();budgetReconnectCredit_=false;clearCaptcha(QString::fromUtf8("已停止，请重新获取"));clearBookingPreview();renewalPending_=false;renewalCredit_=false;sessionObserved_.invalidate();debounce_.stop();pendingLoad_=false;pendingForce_=false;cancel_->store(true);ready_=false;lastQuerySuccess_=false;warmExpiry_.stop();repeat_->setChecked(false);connection_->setText(QString::fromUtf8("站点连接：已停止/需重新校验"));status_->setText(QString::fromUtf8("正在取消并关闭后台会话…"));updateControls();}
 void completed(QString op,QJsonObject result){
  if(op.startsWith("booking_")){bookingCompleted(op,result);return;}
  if(closing_)return;if(cancel_->load()&&busy_&&op!="prewarm"){result["status"]="cancelled";result["business_success"]=false;result["failure"]="cancelled_by_user";result["rows"]=QJsonArray();}last_=result;QString status=result.value("status").toString();bool ok=status=="complete"||status=="ready";
  if(op=="prewarm"){sessionHttpUsed_=result.value("runtime").toObject().value("http_dispatches").toInt();warming_=false;ready_=ok&&!cancel_->load();if(ready_){observeSession(result.value("performance").toObject());ready_=sessionAgeMs()<100000&&sessionApiUsed_<8&&sessionHttpUsed_<12;if(ready_)warmExpiry_.start(qMax(1,100000-int(sessionAgeMs())));}else{renewalPending_=false;renewalCredit_=false;sessionObserved_.invalidate();}performance_->setText((ready_?QString::fromUtf8("站点%1完成：%2 秒（连接准备耗时，非号源查询耗时）"):QString::fromUtf8("站点%1未就绪：%2 秒（未发起号源查询）")).arg(automaticWarm_?QString::fromUtf8("自动连接准备"):QString::fromUtf8("自动连接")).arg(warmTimer_.elapsed()/1000.0,0,'f',3));log_->appendPlainText(performance_->text());connection_->setText(ready_?QString::fromUtf8("站点连接：就绪；连接准备 %1 秒").arg(warmTimer_.elapsed()/1000.0,0,'f',3):QString::fromUtf8("站点连接：未就绪，未自动重试"));updateControls();if(ready_&&repeat_->isChecked()&&lastQuerySuccess_)repeatTimer_.start(interval_->value()*1000);if(batchActive_&&waitingConnection_){waitingConnection_=false;if(ready_)dispatchNextDate();else{batchActive_=false;queryDates_.clear();repeat_->setChecked(false);status_->setText(QString::fromUtf8("自动连接失败，已停止本批查询；可点击查询重试。"));updateControls();}return;}if(pendingLoad_){bool force=pendingForce_;pendingLoad_=false;pendingForce_=false;if(ok&&!cancel_->load())loadDepartments(force);else status_->setText(QString::fromUtf8("站点预热未成功，已停止排队请求；请手动重试。"));}return;}
  busy_=false;
   if(op=="availability")sessionHttpUsed_=result.value("runtime").toObject().value("http_dispatches").toInt(sessionHttpUsed_);
   if(op=="availability"&&batchActive_){
    const bool localBudget=result.value("failure")==QJsonValue("public_session_budget_exhausted")&&result.value("session_kept_open")==QJsonValue(true)&&result.value("session_invalidated")==QJsonValue(false);
    if(!ok&&localBudget&&budgetRetryForDate_==0&&!cancel_->load()){
     // This local refusal happens before HTTP dispatch: renew once for this date.
     ++budgetRetryForDate_;ready_=false;waitingConnection_=true;startWarm(true);return;
    }
    if(ok&&jobHospital_==hospital_){
     for(const auto &row:result.value("rows").toArray())queryRows_.append(row);const auto notice=result.value("message").toString();if(!notice.isEmpty())queryNotices_.append(notice);
     observeSession(result.value("performance").toObject());ready_=sessionAgeMs()<100000&&sessionApiUsed_<8&&sessionHttpUsed_<12;
     ++queryDateIndex_;budgetRetryForDate_=0;
     if(queryDateIndex_<queryDates_.size()){renderAvailability(queryRows_);dispatchNextDate();return;}
     result["rows"]=queryRows_;result["queried_date_count"]=queryDateIndex_;if(!queryNotices_.isEmpty())result["message"]=queryNotices_.join(QString::fromUtf8("\n"));batchActive_=false;waitingConnection_=false;
    }else{batchActive_=false;waitingConnection_=false;budgetReconnectCredit_=false;cancelBudgetReconnect();repeat_->setChecked(false);if(!queryRows_.isEmpty())renderAvailability(queryRows_);log_->appendPlainText(QString::fromUtf8("多日期查询中止：已完成 %1 / %2 天；仅显示已完成日期，未自动重发失败请求。").arg(queryDateIndex_).arg(queryDates_.size()));}
   }
   if(op=="crawl_all_departments"){catalogProgressTimer_.stop();if(ok){status_->setText(QString::fromUtf8("全院科室快照已完整发布：%1 家医院；新增网络获取 %2 家。").arg(result.value("hospital_count").toInt()).arg(result.value("network_fetched_hospitals").toInt()));performance_->setText(QString::fromUtf8("全量采集耗时 %1 秒；HTTP %2 次；快照：%3").arg(result.value("elapsed_seconds").toDouble(),0,'f',1).arg(result.value("network_requests").toInt()).arg(result.value("snapshot_path").toString()));if(!hospital_.isEmpty())loadDepartments(false);}else status_->setText(QString::fromUtf8("全院采集%1：%2；已完成 %3 / %4 家，进度可续跑。").arg(status).arg(result.value("failure").toString()).arg(result.value("completed_hospitals").toInt()).arg(result.value("hospital_count").toInt()));log_->appendPlainText(status_->text());updateControls();emit resultRendered(result);return;}if(!ok){if(op=="availability"&&(result.value("failure")==QJsonValue("legacy_hospital_api_required")||result.value("hospital_system")==QJsonValue("legacy"))){queryRows_={};renderAvailability({});}if(op=="availability"){renewalPending_=false;renewalCredit_=false;sessionObserved_.invalidate();ready_=false;warmExpiry_.stop();connection_->setText(QString::fromUtf8("站点连接：未就绪；查询失败，会话已关闭"));}lastQuerySuccess_=false;repeatTimer_.stop();const QString failure=result.value("failure").toString();if(failure=="public_session_budget_exhausted"&&result.value("session_kept_open").toBool()){sessionApiUsed_=8;connection_->setText(QString::fromUtf8("站点连接：本地会话额度用尽，将自动恢复"));}status_->setText((failure=="legacy_hospital_api_required"||result.value("hospital_system")==QJsonValue("legacy"))?result.value("message").toString():failure=="public_session_budget_exhausted"?QString::fromUtf8("本会话额度已用尽，本次查询未完成；自动恢复连接后可再查询。"):failure=="manual_connection_required"?QString::fromUtf8("连接不可用；再次点击查询会自动连接。"):failure=="department_snapshot_unavailable"?QString::fromUtf8("本地科室快照尚未覆盖此医院。请先运行一次“全院科室采集”；加载科室不会联网。"):QString::fromUtf8("任务%1：%2").arg(status,failure));QString officialMessage=result.value("official_message").toString();if(officialMessage.isEmpty())for(const auto &v:result.value("responses").toArray()){const auto response=v.toObject();if(response.value("business_code")!=QJsonValue("0000"))officialMessage=response.value("business_message").toString();}if(!officialMessage.isEmpty())status_->setText(status_->text()+QString::fromUtf8("；官方提示：")+officialMessage.left(160));log_->appendPlainText(status_->text());if(op=="availability")queueBudgetReconnect(result);updateControls();emit resultRendered(result);return;}
  if(jobHospital_!=hospital_){updateControls();return;}
  if(op=="departments"){departments_=result.value("departments").toArray();department_->clear();department_->addItem(QString::fromUtf8("请选择单一科室"));for(const auto&v:departments_){auto d=v.toObject();department_->addItem(d.value("first_department_name").toString()+" / "+d.value("name").toString()+" ["+d.value("code").toString()+"]",d.value("code").toString());department_->setItemData(department_->count()-1,d.value("first_department_code").toString(),Qt::UserRole+1);}status_->setText(QString::fromUtf8("科室加载完成：%1 个；来源：%2。").arg(departments_.size()).arg(result.value("metadata_cache_source").toString()));}
  // Batch row replacement to avoid per-cell control updates and intermediate repaints.
   else{directSubmissionLocked_=false;const auto rows=result.value("rows").toArray();QElapsedTimer renderClock;renderClock.start();renderAvailability(rows);result["ui_render_ms"]=double(renderClock.nsecsElapsed())/1e6;status_->setText(QString::fromUtf8("实时查询完成：%1 天，%2 行；号源随时间变化。").arg(result.value("queried_date_count").toInt(1)).arg(rows.size()));int calendarCount=0;for(const auto &value:rows)if(value.toObject().value("row_kind")==QJsonValue("department_calendar"))++calendarCount;
    if(calendarCount>0)status_->setText(status_->text()+QString::fromUtf8("\n其中 %1 行为官方科室日历状态；官方未返回对应医生明细和余量数字。“无号”为官方当前状态，数量未提供不显示为0。").arg(calendarCount));
    const auto notice=result.value("message").toString();if(!notice.isEmpty())status_->setText(status_->text()+QString::fromUtf8("\n")+notice);}
  auto perf=result.value("performance").toObject();if(perf.value("session_api_used").toInt()>=8){ready_=false;warmExpiry_.stop();connection_->setText(QString::fromUtf8("站点连接：待自动恢复"));}if(op=="availability"){observeSession(perf);if(sessionApiUsed_<8&&sessionHttpUsed_<12&&sessionAgeMs()<100000&&!renewalPending_){ready_=true;warmExpiry_.start(qMax(1,100000-int(sessionAgeMs())));connection_->setText(QString::fromUtf8("站点连接：就绪；后续查询复用连接"));}else{ready_=false;warmExpiry_.stop();connection_->setText(QString::fromUtf8("站点连接：待自动恢复"));}lastQuerySuccess_=true;if(repeat_->isChecked())repeatTimer_.start(interval_->value()*1000);}double ms=jobTimer_.isValid()?jobTimer_.elapsed():0;if(op=="departments"&&selectionTimer_.isValid()){ms=selectionTimer_.elapsed();selectionTimer_.invalidate();}performance_->setText(QString::fromUtf8("界面总耗时 %1 秒；HTTP %2 次；科室缓存 %3").arg(ms/1000.0,0,'f',3).arg(perf.value("http_dispatches").toInt()).arg(perf.value("metadata_cache_hit").toBool()?QString::fromUtf8("命中"):QString::fromUtf8("未命中")));result["connection_phase"]=op=="availability"?(perf.value("http_dispatches").toInt()>=5?QStringLiteral("connection_bootstrap_during_query"):QStringLiteral("ready_session_query")):QStringLiteral("local_metadata");result["ui_elapsed_ms"]=ms;result["department_count"]=departments_.size();if(op=="availability"){if(!preparedConnectionReady())requireManualConnection();queueBudgetReconnect(result);}log_->appendPlainText(status_->text()+" "+performance_->text());updateControls();emit resultRendered(result);
 }
protected:void closeEvent(QCloseEvent *event)override{batchActive_=false;waitingConnection_=false;cancelBudgetReconnect();budgetReconnectCredit_=false;if(thread_.isRunning()){if(!closing_){closing_=true;renewalPending_=false;renewalCredit_=false;cancel_->store(true);debounce_.stop();repeatTimer_.stop();warmExpiry_.stop();captchaExpiry_.stop();emit shutdown();status_->setText(QString::fromUtf8("正在清理后台进程…"));}event->ignore();}else event->accept();}
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
 if(args.contains("--cli")){std::atomic_bool cancelled(false);QString op=arg(args,"--operation","departments");if(op=="crawl_all_departments"){const auto result=Service::crawlAllDepartments(root,&cancelled,arg(args,"--crawl-workers","4").toInt());if(output.isEmpty())return 2;if(!writeJson(output,result))return 3;return result.value("status").toString()=="complete"?0:1;}Service service(root);QJsonObject params{{"hospital_code",arg(args,"--hospital","H02110003B")},{"department_code",arg(args,"--department")},{"first_department_code",arg(args,"--first-department")},{"date",arg(args,"--date",QDate::currentDate().toString("yyyyMMdd"))},{"force_metadata_refresh",args.contains("--force")}};QJsonObject result;try{if(args.contains("--prewarm-first")){QElapsedTimer t;t.start();auto warm=service.execute("prewarm",params,&cancelled);warm["external_wall_ms"]=double(t.nsecsElapsed())/1e6;if(!output.isEmpty())writeJson(output+".prewarm.json",warm);if(warm.value("status").toString()!="ready")throw Failure("prewarm_failed");}QElapsedTimer t;t.start();int n=std::max(1,arg(args,"--benchmark-service","1").toInt());QJsonArray durations;for(int i=0;i<n;++i){QElapsedTimer call;call.start();result=service.execute(op,params,&cancelled);durations.append(double(call.nsecsElapsed())/1e6);}result["external_wall_ms"]=double(t.nsecsElapsed())/1e6;result["call_durations_ms"]=durations;result["implementation"]="C++17 / Qt5 / libcurl; Node.js signing; no Python runtime";service.close();result["cleanup_complete"]=true;}catch(const std::exception&e){service.close();result={{"status","failed"},{"failure",QString::fromUtf8(e.what())},{"cleanup_complete",true}};}if(output.isEmpty())return 2;if(!writeJson(output,result))return 3;return result.value("status").toString()=="complete"||result.value("status").toString()=="ready"?0:1;}
 Window window(root,!args.contains("--no-prewarm"));window.show();
 if(args.contains("--benchmark-startup")&&!args.contains("--benchmark-cache")){QTimer::singleShot(0,&window,[&]{QJsonObject result{{"qt_visible",window.isVisible()},{"internal_elapsed_ms",double(startup.nsecsElapsed())/1e6},{"process_id",double(QCoreApplication::applicationPid())},{"implementation","C++ / Qt5"}};const auto bookingUi=window.bookingUiState();for(auto it=bookingUi.begin();it!=bookingUi.end();++it)result.insert(it.key(),it.value());writeJson(output,result);window.close();});}
 if(args.contains("--benchmark-cached-gui")||args.contains("--benchmark-cache")){QObject::connect(&window,&Window::resultRendered,&window,[&](QJsonObject result){result["qt_visible"]=window.isVisible();result["internal_elapsed_ms"]=double(startup.nsecsElapsed())/1e6;result["cache_click_to_render_ms"]=result.value("ui_elapsed_ms");writeJson(output,result);window.close();});QTimer::singleShot(0,&window,[&]{window.benchmarkSelect(arg(args,"--hospital","H02"));});}return app.exec();
}
#include "main.moc"
