#pragma once

#include <QCoreApplication>
#include <QString>

namespace LauncherPaths {

inline QString RuntimeDirectory(const QString& fallback = QCoreApplication::applicationDirPath()) {
	return qEnvironmentVariable("KYTY_APPIMAGE_DATA_DIR", fallback);
}

} // namespace LauncherPaths
