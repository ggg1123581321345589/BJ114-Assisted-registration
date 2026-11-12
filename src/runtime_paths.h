#pragma once
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>
inline QString resolveNodeExecutable(const QString &root) {
 const auto configured=qEnvironmentVariable("NODE_EXECUTABLE");if(!configured.isEmpty())return QDir::cleanPath(configured);
 const auto bundled=QDir(root).filePath("runtime/node/node.exe");if(QFileInfo::exists(bundled))return bundled;
 return QStandardPaths::findExecutable(QStringLiteral("node"));
}
