// Render the real application without connecting any business execution.
#define main booking_application_main
#include "../src/main.cpp"
#undef main
#include <QTextStream>
#include <cstdio>
class UiProbe {
public:
 static QJsonObject run(const QString &root,const QString &out){
  Window w(root,false);
  bool disconnected=true;int disconnectedWorkers=0;for(int slot=0;slot<16;++slot)if(w.queryWorkers_[slot]){disconnected=QObject::disconnect(&w,&Window::work,w.queryWorkers_[slot],&Worker::execute)&&disconnected;++disconnectedWorkers;}
  QJsonArray work,checks;int failed=0;
  auto check=[&](const char *name,bool pass){checks.append(QJsonObject{{"name",QString::fromLatin1(name)},{"pass",pass}});if(!pass)++failed;};
  QObject::connect(&w,&Window::work,&w,[&](const QString &op,const QJsonObject &){work.append(op);},Qt::DirectConnection);
  w.autoLoad_->setChecked(false);w.debounce_.stop();w.resize(1250,1100);w.show();QApplication::processEvents();
  auto *scroll=qobject_cast<QScrollArea*>(w.centralWidget());
  if(scroll)scroll->verticalScrollBar()->setValue(0);QApplication::processEvents();
  auto *body=w.findChild<QLabel*>("disclaimerAgreement");
  auto *first=scroll&&scroll->widget()&&scroll->widget()->layout()?qobject_cast<QLabel*>(scroll->widget()->layout()->itemAt(0)->widget()):nullptr;
  check("real_window_visible",w.isVisible());
  check("public_hospital_catalog_loaded",w.hospitals_->rowCount()==308);
  check("worker_execution_disconnected",disconnected&&disconnectedWorkers==w.queryConnectionCount_);
  check("title_exact_v1_1_1",w.windowTitle()==QString::fromUtf8("辅助挂号软件 v1.1.1"));
  check("first_line_is_disclaimer_agreement",first&&first->text()==QString::fromUtf8("免责协议"));
  check("disclaimer_named_label_visible",body&&body->isVisible()&&body->wordWrap()&&body->textFormat()==Qt::PlainText);
  check("disclaimer_has_manual_confirmation_and_official_result",body&&body->text().contains(QString::fromUtf8("人工确认"))&&body->text().contains(QString::fromUtf8("官方平台为准")));
  bool removedOfficialLinkAbsent=true;
  for(auto *label:w.findChildren<QLabel*>())if(label->text().contains(QString::fromUtf8("打开官方页面查看协议 / 订单")))removedOfficialLinkAbsent=false;
  check("removed_official_agreement_order_link_absent",removedOfficialLinkAbsent);
  check("phone_is_empty",w.phone_->text().isEmpty());
  check("image_code_is_empty",w.imageCode_->text().isEmpty());
  check("sms_code_is_empty",w.smsCode_->text().isEmpty());
  check("captcha_not_prefilled",!w.captchaLoaded_&&!w.captchaImage_->pixmap());
  check("logged_out_with_no_patients",!w.loggedIn_&&w.bookingPatients_.isEmpty());
  check("manual_booking_button_exists",w.submitBooking_&&!w.submitBooking_->isHidden()&&w.submitBooking_->text().contains(QString::fromUtf8("直接挂号")));
  check("manual_booking_initially_disabled",!w.submitBooking_->isEnabled());
  check("connection_threads_default_and_range",w.connectionThreads_->value()==7&&w.connectionThreads_->minimum()==1&&w.connectionThreads_->maximum()==16);
  check("pool_recovery_idle_before_selection",!w.poolRecoveryEnabled_&&w.poolRecoveryTimer_.isActive());
  check("query_time_beijing_display",queryTimeForDisplay("2026-11-12T03:00:00.000Z")==QStringLiteral("2026-11-12 11:00:00"));
  int scheduledButtons=0,scheduledLabels=0;
  for(auto *b:w.findChildren<QPushButton*>())if(b->text().contains(QString::fromUtf8("定时")))++scheduledButtons;
  for(auto *l:w.findChildren<QLabel*>())if(l->text().contains(QString::fromUtf8("执行时间（本机）"))||l->text().contains(QString::fromUtf8("到点自动")))++scheduledLabels;
  check("scheduled_entries_absent",scheduledButtons==0&&scheduledLabels==0&&w.findChildren<QDateTimeEdit*>().size()==1);
  check("no_work_requests",work.isEmpty());
  QDir().mkpath(QFileInfo(out).absolutePath());const auto folder=QFileInfo(out).absolutePath();
  const QString full=QDir(folder).filePath("PREVIEW_WINDOW.png"),top=QDir(folder).filePath("PREVIEW_DISCLAIMER.png");
  check("window_screenshot_saved",w.grab().save(full));
  const int bottom=body?body->mapTo(&w,QPoint(0,body->height())).y()+12:0;
  check("disclaimer_screenshot_saved",body&&bottom>0&&w.grab(QRect(0,0,w.width(),qMin(bottom,w.height()))).save(top));
  const QString title=w.windowTitle(),firstText=first?first->text():QString(),bodyText=body?body->text():QString();
  w.close();QElapsedTimer timer;timer.start();while(w.queryOwnersRunning()&&timer.elapsed()<10000){QApplication::processEvents();QThread::msleep(2);}
  check("worker_shutdown_complete",!w.queryOwnersRunning());check("no_work_on_shutdown",work.isEmpty());
  return {{"status",failed?"FAIL":"PASS"},{"checks",checks},{"count",checks.size()},{"failed",failed},{"project_root",root},{"window_title",title},{"first_line",firstText},{"disclaimer",bodyText},{"work_operations",work},{"website_requests",0},{"real_order_submissions",0},{"worker_execute_disconnected",disconnected},{"window_screenshot",full},{"disclaimer_screenshot",top}};
 }
};
int main(int argc,char **argv){
 if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))qputenv("QT_QPA_PLATFORM","windows");QApplication app(argc,argv);
 if(argc!=3){QTextStream(stderr)<<"Usage: preview_probe PROJECT_ROOT OUTPUT_JSON"<<Qt::endl;return 2;}
 const auto args=app.arguments();const auto result=UiProbe::run(QDir(args.at(1)).absolutePath(),args.at(2));
 if(!writeJson(args.at(2),result))return 3;const auto bytes=QJsonDocument(result).toJson(QJsonDocument::Compact);fwrite(bytes.constData(),1,bytes.size(),stdout);return result.value("status")==QJsonValue("PASS")?0:1;
}
