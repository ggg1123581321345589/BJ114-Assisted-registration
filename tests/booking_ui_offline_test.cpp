// Offline UI contract probe. The work->Worker connection is removed before actions.
// Reconcile fixture keys with bookingCompleted() after UI integration is complete.
#define main booking_application_main
#include "../src/main.cpp"
#undef main
#include <QTextStream>
#include <cstdio>

class BookingUiProbe {
public:
 static QJsonObject run(const QString &root) {
  Window w(root,false,false);
  for(int slot=0;slot<16;++slot)if(w.queryWorkers_[slot])QObject::disconnect(&w,&Window::work,w.queryWorkers_[slot],&Worker::execute);
  w.autoLoad_->setChecked(false);w.debounce_.stop();w.selectionQueryPending_=false;w.selectionQueryTimer_.stop();w.show();QApplication::processEvents();
  QJsonArray checks,actions; int failed=0;
  auto check=[&](const QString &name,bool pass){QTextStream(stderr)<<"CHECK:"<<name<<":"<<pass<<Qt::endl;checks.append(QJsonObject{{"name",name},{"pass",pass}});if(!pass)++failed;};
  QObject::connect(&w,&Window::work,&w,[&](const QString &op,const QJsonObject &params){actions.append(QJsonObject{{"op",op},{"params",params}});},Qt::DirectConnection);
  check("login_controls_present",w.phone_&&w.imageCode_&&w.smsCode_&&w.getCaptcha_&&w.sendSms_&&w.login_&&w.logout_);
  check("booking_controls_present",w.loadPatients_&&w.previewBooking_&&w.submitBooking_&&w.patient_&&w.card_&&w.terms_&&w.loginState_);
  check("auto_login_disabled_in_offline_test",!w.autoLoginEnabled_&&!w.autoLoginPending_);check("test_session_path_isolated",QStandardPaths::isTestModeEnabled()&&BookingSessionStore::defaultPath().contains(QCoreApplication::applicationName())&&!BookingSessionStore::hasSavedSession());
  check("initially_logged_out",!w.loggedIn_);check("phone_initially_empty",w.phone_->text().isEmpty());check("sms_display_is_normal",w.smsCode_->echoMode()==QLineEdit::Normal);check("phone_display_is_normal",w.phone_->echoMode()==QLineEdit::Normal);check("captcha_shown_at_readable_size",w.captchaImage_->width()>=200&&w.captchaImage_->height()>=100);check("manual_captcha_input_numeric_four",w.imageCode_->maxLength()==4&&w.imageCode_->validator());
  check("no_bootstrap_or_network_queued",actions.isEmpty());

  // Values are synthetic fixtures and are captured only in this offline test.
  w.phone_->setText("13800000000");w.imageCode_->setText("1234");w.smsCode_->setText("123456");check("sms_digits_visible",w.smsCode_->displayText()=="123456");
  check("sms_requires_agreement",!w.sendSms_->isEnabled());w.bookingAction("booking_sms",{{"phone","13800000000"},{"captcha","1234"}});check("direct_sms_requires_agreement",actions.isEmpty());
  w.terms_->setChecked(true);check("sms_requires_loaded_captcha",!w.sendSms_->isEnabled());w.bookingAction("booking_sms",{{"phone","13800000000"},{"captcha","1234"}});check("direct_sms_requires_loaded_image",actions.isEmpty());
  QImage fixture(200,100,QImage::Format_RGB32);fixture.fill(Qt::white);QByteArray fixtureBytes;QBuffer fixtureBuffer(&fixtureBytes);fixtureBuffer.open(QIODevice::WriteOnly);fixture.save(&fixtureBuffer,"PNG");
  auto captchaResult=[&](){return QJsonObject{{"status","complete"},{"logged_in",false},{"captcha_b64",QString::fromLatin1(fixtureBytes.toBase64())}};};
  w.bookingCompleted("booking_captcha",captchaResult());w.imageCode_->setText("1234");check("valid_sms_input_enables_action",w.sendSms_->isEnabled()&&w.captchaLoaded_);check("captcha_timer_started",w.captchaExpiry_.isActive());
  w.bookingAction("booking_captcha",{});check("refresh_clears_old_image_code",!w.captchaLoaded_&&w.imageCode_->text().isEmpty()&&w.captchaImage_->pixmap()==nullptr);check("refresh_shows_loading",w.getCaptcha_->text().contains(QString::fromUtf8("加载"))&&w.captchaImage_->text().contains(QString::fromUtf8("加载")));
  w.bookingCompleted("booking_captcha",{{"status","failed"},{"logged_in",false},{"message","fixture-failed"}});check("failed_refresh_clears_old_image",!w.captchaLoaded_&&w.captchaImage_->pixmap()==nullptr&&!w.sendSms_->isEnabled()&&w.getCaptcha_->isEnabled());
  w.bookingCompleted("booking_captcha",{{"status","complete"},{"logged_in",false},{"captcha_b64","aW52YWxpZA=="}});check("bad_image_does_not_enable_sms",!w.captchaLoaded_&&!w.sendSms_->isEnabled());
  w.bookingCompleted("booking_captcha",captchaResult());w.imageCode_->setText("1234");QMetaObject::invokeMethod(&w.captchaExpiry_,"timeout",Qt::DirectConnection);check("captcha_expiry_invalidates_image",!w.captchaLoaded_&&w.imageCode_->text().isEmpty()&&!w.sendSms_->isEnabled());
  w.bookingCompleted("booking_captcha",captchaResult());w.imageCode_->setText("1234");w.stop();{QElapsedTimer reset;reset.start();while(w.poolResetting()&&reset.elapsed()<5000){QApplication::processEvents();QThread::msleep(2);}}check("stop_invalidates_captcha",!w.captchaLoaded_&&!w.captchaExpiry_.isActive()&&w.imageCode_->text().isEmpty());w.bookingCompleted("booking_captcha",captchaResult());check("late_captcha_after_stop_not_shown",!w.captchaLoaded_&&w.captchaImage_->pixmap()==nullptr);w.cancel_->store(false);w.bookingCompleted("booking_captcha",captchaResult());w.imageCode_->setText("1234");
  const int beforeSms=actions.size();w.bookingAction("booking_sms",{{"phone","13800000000"},{"captcha","1234"}});
  check("action_captured_without_worker_dispatch",actions.size()==beforeSms+1);check("sms_consumes_captcha_image",!w.captchaLoaded_&&w.imageCode_->text().isEmpty()&&!w.captchaExpiry_.isActive());
  if(!actions.isEmpty()){auto a=actions.last().toObject();check("sms_action_preserves_operation",a.value("op")==QJsonValue("booking_sms"));check("sms_action_preserves_payload",a.value("params").toObject().value("phone")==QJsonValue("13800000000"));}
  w.bookingCompleted("booking_sms",{{"status","complete"},{"retry_after_seconds",60},{"logged_in",false}});
  check("sms_cooldown_disables_resend",w.smsSeconds_==60&&!w.sendSms_->isEnabled());


  w.bookingCompleted("booking_login",{{"status","complete"},{"logged_in",true},{"message","offline fixture"}});
  check("login_completion_updates_state",w.loggedIn_);w.parallelSession_={{"offline_fixture",true}};w.availabilityStale_=false;
  QJsonArray patients{QJsonObject{{"patient_id","fixture-p1"},{"name","测试就诊人甲"},{"cards",QJsonArray{QJsonObject{{"card_index",0},{"label","甲卡0000"}},QJsonObject{{"card_index",4},{"label","甲卡0004"}}}}},QJsonObject{{"patient_id","fixture-p2"},{"name","测试就诊人乙"},{"cards",QJsonArray{QJsonObject{{"card_index",0},{"label","乙卡0000"}},QJsonObject{{"card_index",42},{"label","乙医保卡0042"}}}}}};
  auto selectPatientCard=[&](){w.patient_->setCurrentIndex(w.patient_->findData("fixture-p2"));w.card_->setCurrentIndex(w.card_->findData(42));w.updateControls();};
  w.populateBookingPatients(patients);selectPatientCard();
  check("patient_projection_retained",w.bookingPatients_.size()==2);
  check("selected_patient_bound",w.patient_->currentData().toString()=="fixture-p2");
  check("selected_card_bound",w.card_->currentData().toInt()==42);
  check("no_row_disables_direct",!w.submitBooking_->isEnabled());
  const QJsonObject payload{{"hosCode","H01110005"},{"firstDeptCode","first"},{"secondDeptCode","second"},{"dutyDate","20261116"},{"uniqueProductKey","fixture-key"},{"uniqueProductTimeKey","fixture-time"},{"doctorCode","fixture-doctor"},{"sourcePage","list"}};
  const QJsonObject row{{"booking_payload",payload},{"product_status",1},{"date","20261116"},{"department_name","测试科室"},{"doctor_name","测试医生"},{"period","10:30-11:00"}};
  auto setRow=[&](const QJsonObject &value){w.rows_->setRowCount(1);if(!w.rows_->item(0,0))w.rows_->setItem(0,0,new QTableWidgetItem("fixture"));w.rows_->item(0,0)->setData(Qt::UserRole,value);w.rows_->setCurrentCell(0,0);w.rows_->selectRow(0);w.updateControls();};
  setRow(row);w.clearBookingPreview();w.updateControls();
  check("manual_preview_hidden",w.previewBooking_->isHidden());
  check("direct_label_visible",w.submitBooking_->text().contains(QString::fromUtf8("直接挂号")));
  check("direct_enabled_without_preview_nonce",w.bookingPreviewId_.isEmpty()&&w.submitBooking_->isEnabled());
  w.card_->setCurrentIndex(0);check("missing_card_disables_direct",!w.submitBooking_->isEnabled());selectPatientCard();
  w.patient_->setCurrentIndex(0);check("missing_patient_disables_direct",!w.submitBooking_->isEnabled());selectPatientCard();
  auto unavailable=row;unavailable["product_status"]=0;setRow(unavailable);check("unavailable_row_disables_direct",!w.submitBooking_->isEnabled());setRow(row);
  auto bad=row;bad.remove("booking_payload");setRow(bad);check("missing_payload_disables_direct",!w.submitBooking_->isEnabled());setRow(row);
  w.loggedIn_=false;w.updateControls();check("logged_out_disables_direct",!w.submitBooking_->isEnabled());w.loggedIn_=true;w.updateControls();
  w.busy_=true;w.updateControls();check("busy_disables_direct",!w.submitBooking_->isEnabled());w.busy_=false;w.updateControls();
  check("parallel_booking_cap_default_16",w.bookingParallelThreads_->value()==16&&w.bookingParallelThreads_->minimum()==1&&w.bookingParallelThreads_->maximum()==16);
  check("missing_parallel_session_disables_direct",([&]{auto snapshot=w.parallelSession_;w.parallelSession_={};w.updateControls();bool disabled=!w.submitBooking_->isEnabled();w.parallelSession_=snapshot;w.updateControls();return disabled;})());
  w.availabilityStale_=true;w.updateControls();check("stale_availability_blocks_direct",!w.submitBooking_->isEnabled());w.availabilityStale_=false;w.updateControls();
  // Occupy all owner slots with never-started fixture objects. This holds the real
  // application queue at its configured cap; no DirectBookingThread is executed.
  for(quint64 id=100;id<116;++id){auto *thread=new DirectBookingThread(root,{}, {},std::make_shared<std::atomic_bool>(false),id,w.bookingAuthEpoch_,&w);w.directTasks_.insert(id,{thread,std::make_shared<std::atomic_bool>(false),w.bookingAuthEpoch_,false});}
  w.directBookingActive_=true;w.busy_=true;w.operation_="booking_direct";w.cancel_->store(false);w.updateControls();
  check("manual_button_accepts_independent_click_while_parallel_running",w.submitBooking_->isEnabled());
  const int beforeClicks=actions.size();const auto beforeId=w.directBookingRequestId_;w.submitBooking_->click();
  check("manual_click_queues_exactly_one_request",w.directBookingQueue_.size()==1&&w.directBookingRequestId_==beforeId+1);
  const auto queued=w.directBookingQueue_.isEmpty()?QJsonObject():w.directBookingQueue_.head();
  check("queued_request_binds_current_row_patient_card",queued.value("row").toObject()==row&&queued.value("patient_id")==QJsonValue("fixture-p2")&&queued.value("card_index").toInt(-1)==42&&queued.value("confirmed").toBool()&&!queued.contains("preview_id"));
  w.submitBooking_->click();check("second_manual_click_has_independent_request_id",w.directBookingQueue_.size()==2&&w.directBookingRequestId_==beforeId+2&&w.directBookingQueue_.first().value("_booking_request_id")!=w.directBookingQueue_.last().value("_booking_request_id"));
  check("parallel_limit_holds_queue_without_new_owner",w.directTasks_.size()==16&&w.directBookingQueue_.size()==2);
  check("parallel_path_never_dispatches_shared_owner_work",actions.size()==beforeClicks);
  check("patient_card_remain_editable_during_parallel_tasks",w.patient_->isEnabled()&&w.card_->isEnabled());
  const auto summaryBefore=w.bookingSummary_->text();w.parallelBookingCompleted(999,w.bookingAuthEpoch_,{{"status","complete"},{"order_id",999}});check("unknown_parallel_result_ignored",w.bookingSummary_->text()==summaryBefore);
  w.parallelBookingCompleted(100,w.bookingAuthEpoch_+1,{{"status","complete"},{"order_id",999}});check("mismatched_epoch_result_ignored",!w.directTasks_[100].resultSeen&&w.bookingSummary_->text()==summaryBefore);
  w.parallelBookingCompleted(100,w.bookingAuthEpoch_,{{"status","complete"},{"order_id",12345},{"submit_attempted",true}});check("matching_parallel_order_result_displayed",w.directTasks_[100].resultSeen&&w.bookingSummary_->text().contains("12345"));
  const auto completedSummary=w.bookingSummary_->text();w.parallelBookingCompleted(100,w.bookingAuthEpoch_,{{"status","complete"},{"order_id",54321}});check("duplicate_parallel_result_ignored",w.bookingSummary_->text()==completedSummary);
  w.parallelBookingCompleted(101,w.bookingAuthEpoch_,{{"status","outcome_unknown"},{"outcome_unknown",true}});check("unknown_result_directs_official_order_check_without_retry",w.bookingSummary_->text().contains(QString::fromUtf8("官方订单"))&&w.bookingSummary_->text().contains(QString::fromUtf8("不会自动重发"))&&w.directBookingQueue_.size()==2);
  w.cancelDirectTasks();bool cancelled=true;for(const auto &task:w.directTasks_)cancelled=cancelled&&task.cancel->load();check("cancel_clears_pending_queue_and_marks_all_owners",w.directBookingQueue_.isEmpty()&&cancelled);
  for(const auto &task:w.directTasks_)delete task.thread;w.directTasks_.clear();w.directBookingActive_=false;w.busy_=false;w.operation_.clear();w.updateControls();
  check("parallel_fixture_owners_released",w.directTasks_.isEmpty()&&w.directBookingQueue_.isEmpty());
  const auto epoch=w.bookingAuthEpoch_;w.directTasks_.insert(200,{nullptr,std::make_shared<std::atomic_bool>(false),epoch,false});
  w.parallelBookingCompleted(200,epoch,{{"status","failed"},{"failure","session_expired"},{"logged_in",false}});
  check("expired_parallel_session_clears_login_and_snapshot",!w.loggedIn_&&w.parallelSession_.isEmpty()&&w.bookingPatients_.isEmpty());
  check("expired_session_forget_is_tagged_once",w.ownerForgetPending_&&actions.size()==beforeClicks+1&&actions.last().toObject().value("op")==QJsonValue("booking_forget_session")&&actions.last().toObject().value("params").toObject().value("_auth_epoch").toDouble()==double(epoch+1));
  w.directTasks_.clear();w.bookingCompleted("booking_forget_session",{{"status","complete"},{"logged_in",false},{"_auth_epoch",double(epoch)}});check("stale_forget_result_does_not_unlock_new_epoch",w.ownerForgetPending_);
  w.bookingCompleted("booking_forget_session",{{"status","complete"},{"logged_in",false},{"_auth_epoch",double(epoch+1)}});check("current_forget_result_unlocks_owner",!w.ownerForgetPending_);
  w.loggedIn_=true;w.parallelSession_={{"offline_fixture",true}};w.populateBookingPatients(patients);selectPatientCard();w.availabilityStale_=false;setRow(row);
  auto waiting=row;waiting["product_status"]=8;setRow(waiting);const auto waitlistUrl=w.officialWaitlistUrl(waiting);check("official_waitlist_route_is_explicit_and_validated",w.waitlistBooking_->isEnabled()&&waitlistUrl.scheme()=="https"&&waitlistUrl.host()=="www.114yygh.com"&&waitlistUrl.path()=="/newhlwyl/mobile/appointmentRegister");
  auto invalidWaiting=waiting;auto invalidRoute=payload;invalidRoute["hosCode"]="../invalid";invalidWaiting["booking_payload"]=invalidRoute;check("waitlist_rejects_invalid_route",w.officialWaitlistUrl(invalidWaiting).isEmpty());setRow(row);
  const QString privateMessage="fixture-private-message";w.bookingCompleted("booking_patients",{{"status","failed"},{"message",privateMessage},{"logged_in",true}});
  check("server_message_visible",w.loginState_->text()==privateMessage);check("server_message_not_logged",!w.log_->toPlainText().contains(privateMessage));
  setRow(row);selectPatientCard();QApplication::processEvents();const QString screenshot=QDir(QCoreApplication::applicationDirPath()).filePath("UI_REMOVED.png");check("screenshot_written",w.grab().save(screenshot));

  // Reused fixture with synthetic injection only; Worker::execute remains disconnected.
  int scheduleButtons=0,scheduleLabels=0;QStringList scheduleTexts;
  for(auto*b:w.findChildren<QPushButton*>())if(b->text()==QString::fromUtf8("设置定时挂号")||b->text()==QString::fromUtf8("取消定时")){++scheduleButtons;scheduleTexts.append(b->text());}
  for(auto*l:w.findChildren<QLabel*>())if(l->text().contains(QString::fromUtf8("执行时间（本机）"))||l->text().contains(QString::fromUtf8("到点自动核验并提交一次"))){++scheduleLabels;scheduleTexts.append(l->text());}
  const int dateTimeEditors=w.findChildren<QDateTimeEdit*>().size();
  QJsonObject scheduledObservation{{"buttons",scheduleButtons},{"labels",scheduleLabels},{"datetime_editors",dateTimeEditors},{"texts",QJsonArray::fromStringList(scheduleTexts)}};
  check("scheduled_buttons_absent",scheduleButtons==0);
  check("scheduled_labels_absent",scheduleLabels==0);
  check("scheduled_datetime_input_absent",dateTimeEditors==1);

  check("query_calendar_still_present",w.calendar_&&w.query_&&w.repeat_);
  w.calendar_->setDisplayedMonth(QDate(2026,11,1));w.calendar_->setSelectedDates({QDate(2026,11,16),QDate(2026,11,17)});
  check("calendar_retains_multiple_selected_dates",w.calendar_->selectedDates()==QList<QDate>{QDate(2026,11,16),QDate(2026,11,17)});
  auto timed=row;timed["queried_at"]="2026-11-12T03:01:08.659Z";timed["doctor_title"]=QString::fromUtf8("主任医师");timed["registration_fee"]=QStringLiteral("50.5");w.renderAvailability(QJsonArray{timed});
  check("nine_column_doctor_title_and_fee_projection",w.rows_->columnCount()==9&&w.rows_->item(0,3)->text()==QString::fromUtf8("主任医师")&&w.rows_->item(0,5)->text()==QStringLiteral("50.50"));
  check("query_time_column_displays_beijing_and_raw_tooltip",w.rows_->horizontalHeaderItem(8)->text()==QString::fromUtf8("查询时间（北京时间）")&&w.rows_->item(0,8)->text()==QStringLiteral("2026-11-12 11:01:08")&&w.rows_->item(0,8)->toolTip()==QStringLiteral("2026-11-12T03:01:08.659Z"));
  auto *retainedItem=w.rows_->item(0,8);timed["queried_at"]="2026-11-12T03:02:00.000Z";w.renderAvailability(QJsonArray{timed});
  check("incremental_refresh_updates_time_and_tooltip_in_place",w.rows_->item(0,8)==retainedItem&&retainedItem->text()==QStringLiteral("2026-11-12 11:02:00")&&retainedItem->toolTip()==QStringLiteral("2026-11-12T03:02:00.000Z"));
  timed["row_kind"]="department_calendar";timed["remaining"]=QJsonValue(QJsonValue::Null);w.renderAvailability(QJsonArray{timed});
  check("calendar_summary_time_tooltip_preserves_raw_timestamp",w.rows_->item(0,8)->toolTip()==QStringLiteral("2026-11-12T03:02:00.000Z"));
  check("ambiguous_timezone_is_not_invented",queryTimeForDisplay(QStringLiteral("2026-11-12T03:00:00"))==QString::fromUtf8("未知"));
  w.renderAvailability(QJsonArray{row});auto*day=w.calendar_->findChild<QAbstractButton*>("calendarDay_20261116");
  check("calendar_doctor_annotation_retained",day&&day->property("doctorNames").toStringList().contains(QString::fromUtf8("测试医生")));
  auto legacy=row;legacy["booking_supported"]=false;legacy["read_only"]=true;legacy["row_kind"]="legacy_doctor";legacy["remaining"]=QJsonValue(QJsonValue::Null);legacy["status"]=QString::fromUtf8("约满");
  w.renderAvailability(QJsonArray{legacy});w.rows_->setCurrentCell(0,0);w.rows_->selectRow(0);selectPatientCard();
  check("legacy_readonly_blocks_direct_despite_valid_payload",!w.submitBooking_->isEnabled());
  check("legacy_readonly_blocks_preview_despite_valid_payload",!w.previewBooking_->isEnabled());
  check("readonly_unknown_remaining_and_sold_out_label_retained",w.rows_->item(0,6)->text()==QString::fromUtf8("未知")&&w.rows_->item(0,7)->text()==QString::fromUtf8("约满"));
  const int beforeReadonly=actions.size();w.submitBooking_->click();check("legacy_direct_click_emits_no_work",actions.size()==beforeReadonly);
  setRow(row);selectPatientCard();
  w.bookingCompleted("booking_logout",{{"status","complete"},{"logged_in",false}});
  check("logout_clears_state",!w.loggedIn_&&w.bookingPreviewId_.isEmpty());check("logout_clears_patients_codes",w.bookingPatients_.isEmpty()&&w.smsCode_->text().isEmpty()&&w.imageCode_->text().isEmpty());check("logout_disables_direct",!w.submitBooking_->isEnabled());
  w.close();QElapsedTimer cleanup;cleanup.start();while(w.queryOwnersRunning()&&cleanup.elapsed()<10000){QApplication::processEvents();QThread::msleep(2);}
  return {{"status",failed?"FAIL":"PASS"},{"checks",checks},{"failed",failed},{"captured_actions",actions},{"scheduled_ui_observation",scheduledObservation},{"website_requests",0},{"worker_execute_disconnected",true}};
 }
};
class UiProbe {public:static int captureCleanPanel(const QString &root,const QString &out){
 Window w(root,false,false);for(int slot=0;slot<16;++slot)if(w.queryWorkers_[slot])QObject::disconnect(&w,&Window::work,w.queryWorkers_[slot],&Worker::execute);QJsonArray work;QObject::connect(&w,&Window::work,&w,[&](const QString&o,QJsonObject){work.append(o);});
 w.autoLoad_->setChecked(false);w.debounce_.stop();w.resize(1250,1100);w.show();QApplication::processEvents();
 for(auto*scroll:w.findChildren<QScrollArea*>()){scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());}QApplication::processEvents();
 QGroupBox *panel=nullptr;for(auto*g:w.findChildren<QGroupBox*>())if(g->isAncestorOf(w.submitBooking_))panel=g;
 const bool saved=panel&&panel->grab().save(out);QJsonObject result{{"status",saved?"PASS":"FAIL"},{"screenshot",out},{"phone_empty",w.phone_->text().isEmpty()},{"logged_in",w.loggedIn_},{"patient",w.patient_->currentText()},{"card",w.card_->currentText()},{"direct_button_text",w.submitBooking_->text()},{"summary",w.bookingSummary_->text()},{"work_operations",work},{"network_requests",0}};
 w.close();QElapsedTimer cleanup;cleanup.start();while(w.queryOwnersRunning()&&cleanup.elapsed()<10000){QApplication::processEvents();QThread::msleep(2);}QTextStream(stdout)<<QJsonDocument(result).toJson(QJsonDocument::Indented);return saved&&work.isEmpty()&&w.phone_->text().isEmpty()?0:1;
 }};
int main(int argc,char **argv){if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))qputenv("QT_QPA_PLATFORM","windows");QTemporaryDir isolated;if(!isolated.isValid())return 4;qputenv("APPDATA",isolated.path().toUtf8());qputenv("LOCALAPPDATA",isolated.path().toUtf8());QStandardPaths::setTestModeEnabled(true);QApplication app(argc,argv);QCoreApplication::setOrganizationName("HospitalBookingOfflineTests");QCoreApplication::setApplicationName("booking_"+QFileInfo(isolated.path()).fileName());if(app.arguments().contains("--clean-panel"))return UiProbe::captureCleanPanel(app.arguments().at(1),QDir(QCoreApplication::applicationDirPath()).filePath("BOOKING_PANEL_REMOVED.png"));QString root=app.arguments().size()>1?app.arguments().at(1):QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("..");auto result=BookingUiProbe::run(root);const auto bytes=QJsonDocument(result).toJson(QJsonDocument::Compact);fwrite(bytes.constData(),1,bytes.size(),stdout);return result.value("status")==QJsonValue("PASS")?0:1;}
