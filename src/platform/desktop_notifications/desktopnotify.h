/*
    Copyright © 2019 by The qTox Project Contributors

    This file is part of qTox, a Qt-based graphical interface for Tox.

    qTox is libre software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    qTox is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with qTox.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "src/model/notificationdata.h"

#include <libsnore/snore.h>

#include <QObject>

#include <memory>
#include <unordered_set>

class Settings;

class DesktopNotify : public QObject
{
    Q_OBJECT
public:
    explicit DesktopNotify(Settings& settings);
    QString sanitizeTextForNotifications(const QString& input_text);

public slots:
    void notifyMessage(const NotificationData& notificationData);

signals:
    void notificationClosed();

private slots:
    void onNotificationClose(Snore::Notification notification);

private:
    Snore::SnoreCore& notifyCore;
    Snore::Application snoreApp;
    Snore::Icon snoreIcon;
    Snore::Notification lastNotification;
    uint latestId;
    Settings& settings;
};
