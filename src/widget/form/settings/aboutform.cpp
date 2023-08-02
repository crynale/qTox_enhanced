/*
    Copyright © 2014-2019 by The qTox Project Contributors

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

#include "aboutform.h"
#include "ui_aboutsettings.h"

#include "src/net/updatecheck.h"
#include "src/persistence/profile.h"
#include "src/persistence/settings.h"
#include "src/widget/style.h"
#include "src/widget/tool/recursivesignalblocker.h"
#include "src/widget/translator.h"
#include "src/widget/widget.h"

#include <tox/tox.h>

#include <libavutil/avutil.h>

#ifndef __STDC_VERSION__
#define __STDC_VERSION__ 199401L
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wall"
#include <opus.h>
// #undef __STDC_VERSION__ // <-- this causes a warning and therefore an error again
#pragma GCC diagnostic pop
#else
#include <opus.h>
#endif

#include <sodium.h>

#include <QDebug>
#include <QDesktopServices>
#include <QPushButton>
#include <QTimer>

#include <memory>

// index of UI in the QStackedWidget
enum class updateIndex
{
    available = 0,
    upToDate = 1,
    failed = 2
};

/**
 * @class AboutForm
 *
 * This form contains information about qTox and libraries versions, external
 * links and licence text. Shows progress during an update.
 */

/**
 * @brief Constructor of AboutForm.
 */
AboutForm::AboutForm(UpdateCheck* updateCheck_, Style& style_)
    : GenericForm(QPixmap(":/img/settings/general.png"), style_)
    , bodyUI(new Ui::AboutSettings)
    , progressTimer(new QTimer(this))
    , updateCheck(updateCheck_)
    , style{style_}
{
    bodyUI->setupUi(this);

    bodyUI->updateStack->setVisible(false);
    bodyUI->unstableVersion->setVisible(false);

    // block all child signals during initialization
    const RecursiveSignalBlocker signalBlocker(this);

    replaceVersions();

    if (QString(GIT_VERSION).indexOf(" ") > -1)
        bodyUI->gitVersion->setOpenExternalLinks(false);

    eventsInit();
    Translator::registerHandler(std::bind(&AboutForm::retranslateUi, this), this);
}

/**
 * @brief Update versions and links.
 *
 * Update commit hash if built with git, show author and known issues info
 * It also updates qTox, toxcore and Qt versions.
 */
void AboutForm::replaceVersions()
{
    // TODO: When we finally have stable releases: build-in a way to tell
    // nightly builds from stable releases.

    QString TOXCORE_VERSION = QString::number(tox_version_major()) + "."
                              + QString::number(tox_version_minor()) + "."
                              + QString::number(tox_version_patch());

    bodyUI->youAreUsing->setText(tr("You are using qTox version %1.").arg(QString(GIT_DESCRIBE)));

    qDebug() << "AboutForm not showing updates, qTox built without UPDATE_CHECK";

    QString commitLink = "https://github.com/Zoxcore/qTox_enhanced/commit/" + QString(GIT_VERSION);
    bodyUI->gitVersion->setText(
        tr("Commit hash: %1").arg(createLink(commitLink, QString(GIT_VERSION))));

    char libavutil_version_str[2000];
    memset(libavutil_version_str, 0, 2000);
    snprintf(libavutil_version_str, 1999, "%d.%d.%d", (int)LIBAVUTIL_VERSION_MAJOR, (int)LIBAVUTIL_VERSION_MINOR, (int)LIBAVUTIL_VERSION_MICRO);

    bodyUI->toxCoreVersion->setText(tr("toxcore version: %1").arg(TOXCORE_VERSION));
    bodyUI->qtVersion->setText(QString("Qt compiled: ") +
        QString(QT_VERSION_STR) +
        QString(" / runtime: ") +
        QString::fromUtf8(qVersion()) +
        QString("\nSQLCipher: ") +
        Widget::sqlcipher_version +
        QString("\nlibav: ") +
        QString::fromUtf8(libavutil_version_str) +
        QString("\nopus: ") +
        QString::fromUtf8(opus_get_version_string()) +
        QString("\nsodium: ") +
        QString::fromUtf8(sodium_version_string())
        );

    qDebug() << "sqlcipher_version:" << Widget::sqlcipher_version;

    QString issueBody = QString("##### Brief Description\n\n"
                                "OS: %1\n"
                                "qTox version: %2\n"
                                "Commit hash: %3\n"
                                "toxcore: %4\n"
                                "Qt: %5\n…\n\n"
                                "Reproducible: Always / Almost Always / Sometimes"
                                " / Rarely / Couldn't Reproduce\n\n"
                                "##### Steps to reproduce\n\n"
                                "1. \n2. \n3. …\n\n"
                                "##### Observed Behavior\n\n\n"
                                "##### Expected Behavior\n\n\n"
                                "##### Additional Info\n"
                                "(links, images, etc go here)\n\n"
                                "----\n\n"
                                "More information on how to write good bug reports in the wiki: "
                                "https://github.com/qTox/qTox/wiki/Writing-Useful-Bug-Reports.\n\n"
                                "Please remove any unnecessary template section before submitting.")
                            .arg(QSysInfo::prettyProductName(), GIT_DESCRIBE, GIT_VERSION,
                                 TOXCORE_VERSION, QT_VERSION_STR);

    issueBody.replace("#", "%23").replace(":", "%3A");

    bodyUI->knownIssues->setText(
        tr("A list of all known issues may be found at our %1 at Github."
           " If you discover a bug or security vulnerability within"
           " qTox, please report it according to the guidelines in our"
           " %2 wiki article.",

           "`%1` is replaced by translation of `bug tracker`"
           "\n`%2` is replaced by translation of `Writing Useful Bug Reports`")
            .arg(createLink("https://github.com/Zoxcore/qTox_enhanced/issues",
                            tr("bug-tracker", "Replaces `%1` in the `A list of all known…`")))
            .arg(createLink("https://github.com/qTox/qTox/wiki/Writing-Useful-Bug-Reports",
                            tr("Writing Useful Bug Reports",
                               "Replaces `%2` in the `A list of all known…`"))));

    bodyUI->clickToReport->setText(
        createLink("https://github.com/Zoxcore/qTox_enhanced/issues/new?body=" + QString::fromUtf8(QUrl(issueBody).toEncoded()),
                   QString("<b>%1</b>").arg(tr("Click here to report a bug."))));


    QString authorInfo =
        QString("<p>%1</p><p>%2</p>")
            .arg(tr("Original author: %1").arg(createLink("https://github.com/tux3", "tux3")))
            .arg(
                tr("See a full list of %1 at Github",
                   "`%1` is replaced with translation of word `contributors`")
                    .arg(createLink("https://qtox.github.io/gitstats/authors.html",
                                    tr("contributors", "Replaces `%1` in `See a full list of…`"))));

    bodyUI->authorInfo->setText(authorInfo);
}

void AboutForm::onUpdateAvailable(QString latestVersion, QUrl link)
{
    std::ignore = latestVersion;
    QObject::disconnect(linkConnection);
    linkConnection = connect(bodyUI->updateAvailableButton, &QPushButton::clicked,
                             [link]() { QDesktopServices::openUrl(link); });
    bodyUI->updateStack->setCurrentIndex(static_cast<int>(updateIndex::available));
}

void AboutForm::onUpToDate()
{
    bodyUI->updateStack->setCurrentIndex(static_cast<int>(updateIndex::upToDate));
}

void AboutForm::onUpdateCheckFailed()
{
    bodyUI->updateStack->setCurrentIndex(static_cast<int>(updateIndex::failed));
}

void AboutForm::reloadTheme()
{
    replaceVersions();
}

void AboutForm::onUnstableVersion()
{
    bodyUI->updateStack->hide();
    bodyUI->unstableVersion->setVisible(true);
}

/**
 * @brief Creates hyperlink with specific style.
 * @param path The URL of the page the link goes to.
 * @param text Text, which will be clickable.
 * @return Hyperlink to paste.
 */
QString AboutForm::createLink(QString path, QString text) const
{
    return QString::fromUtf8(
               "<a href=\"%1\" style=\"text-decoration: underline; color:%2;\">%3</a>")
        .arg(path, style.getColor(Style::ColorPalette::Link).name(), text);
}

AboutForm::~AboutForm()
{
    Translator::unregister(this);
    delete bodyUI;
}

/**
 * @brief Retranslate all elements in the form.
 */
void AboutForm::retranslateUi()
{
    bodyUI->retranslateUi(this);
    replaceVersions();
}
