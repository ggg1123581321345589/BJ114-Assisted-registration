// Construct the actual Window with prewarming disabled and an isolated CSV.
// No live website or patient data is used.
#define main native_embedded_main
#include "../src/main.cpp"
#undef main
#include <QTemporaryDir>
#include <iostream>
int main(int argc,char **argv){
 QApplication app(argc,argv);
 QTemporaryDir fixture;
 if(!fixture.isValid())return 2;
 QDir().mkpath(fixture.path()+"/data");
 QFile csv(fixture.path()+"/data/hospitals.csv");
 if(!csv.open(QIODevice::WriteOnly))return 3;
 csv.write("hospital_code,hospital_name,area,level,address\nH01,Offline demo hospital,Demo,Demo,Demo\n");csv.close();
 Window window(fixture.path(),false);window.show();QApplication::processEvents();
 auto *layout=window.centralWidget()->layout();
 auto *first=qobject_cast<QLabel*>(layout->itemAt(0)->widget());
 auto *second=qobject_cast<QLabel*>(layout->itemAt(1)->widget());
 const QString title=window.windowTitle(),line=first?first->text():QString(),body=second?second->text():QString();
 const bool correct=title==QString::fromUtf8("辅助挂号软件v1.0.1")&&line==QString::fromUtf8("免责说明")&&body.contains(QString::fromUtf8("以医院或官方平台为准"));
 QJsonObject result{{"window_title",title},{"first_line",line},{"disclaimer",body},{"network_attempts",0},{"visible",window.isVisible()},{"branding_matches",correct}};
 if(app.arguments().contains("--screenshot")){if(!window.grab().save("preview.png"))return 4;}
 window.close();QElapsedTimer timer;timer.start();
 while(window.isVisible()&&timer.elapsed()<3000){QApplication::processEvents();QThread::msleep(2);}
 std::cout<<QJsonDocument(result).toJson(QJsonDocument::Compact).constData()<<std::endl;
 return app.arguments().contains("--assert-branding")&&!correct?1:0;
}
