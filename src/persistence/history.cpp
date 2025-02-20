/*
    Copyright © 2015-2019 by The qTox Project Contributors

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

#include <QCoreApplication>
#include <QDebug>
#include <QNetworkAccessManager>
#if QT_VERSION >= QT_VERSION_CHECK( 5, 10, 0 )
#include <QRandomGenerator>
#endif
#include <QUrlQuery>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <cassert>

#include "history.h"
#include "profile.h"
#include "src/core/icoresettings.h"
#include "settings.h"
#include "db/upgrades/dbupgrader.h"
#include "db/rawdatabase.h"
#include "src/core/toxpk.h"
#include "src/core/chatid.h"

// zoff
#include <curl/curl.h>
// zoff

namespace {
MessageState getMessageState(bool isPending, bool isBroken)
{
    assert(!(isPending && isBroken));
    MessageState messageState;

    if (isPending) {
        messageState = MessageState::pending;
    } else if (isBroken) {
        messageState = MessageState::broken;
    } else {
        messageState = MessageState::complete;
    }
    return messageState;
}

void addAuthorIdSubQuery(QString& queryString, QVector<QByteArray>& boundParams, const ToxPk& authorPk)
{
    boundParams.append(authorPk.getByteArray());
    queryString += "(SELECT id FROM authors WHERE public_key = ?)";
}

void addChatIdSubQuery(QString& queryString, QVector<QByteArray>& boundParams, const ChatId& chatId)
{
    boundParams.append(chatId.getByteArray());
    queryString += "(SELECT id FROM chats WHERE uuid = ?)";
}

RawDatabase::Query generateEnsurePkInChats(ChatId const& id)
{
    return RawDatabase::Query{QStringLiteral("INSERT OR IGNORE INTO chats (uuid) "
                                "VALUES (?)"), {id.getByteArray()}};
}

RawDatabase::Query generateEnsurePkInAuthors(ToxPk const& pk)
{
    return RawDatabase::Query{QStringLiteral("INSERT OR IGNORE INTO authors (public_key) "
                                "VALUES (?)"), {pk.getByteArray()}};
}

RawDatabase::Query generateUpdateAlias(ToxPk const& pk, QString const& dispName)
{
    QVector<QByteArray> boundParams;
    QString queryString = QStringLiteral(
            "INSERT OR IGNORE INTO aliases (owner, display_name) VALUES (");
    addAuthorIdSubQuery(queryString, boundParams, pk);
    queryString += ", ?);";
    boundParams += dispName.toUtf8();
    return RawDatabase::Query{queryString, boundParams};
}

RawDatabase::Query generateHistoryTableInsertion(char type, const QDateTime& time, const ChatId& chatId)
{
    QVector<QByteArray> boundParams;
    QString queryString = QStringLiteral("INSERT INTO history (message_type, timestamp, chat_id) "
                                      "VALUES ('%1', %2, ")
                                      .arg(type)
                                      .arg(time.toMSecsSinceEpoch());
    addChatIdSubQuery(queryString, boundParams, chatId);
    queryString += ");";
    return RawDatabase::Query(queryString, boundParams);
}

/**
 * @brief Generate query to insert new message in database
 * @param ChatId Chat ID to save.
 * @param message Message to save.
 * @param sender Sender to save.
 * @param time Time of message sending.
 * @param isDelivered True if message was already delivered.
 * @param dispName Name, which should be displayed.
 * @param insertIdCallback Function, called after query execution.
 */
QVector<RawDatabase::Query>
generateNewTextMessageQueries(const ChatId& chatId, const QString& message, const ToxPk& sender,
                              const QDateTime& time, bool isDelivered, ExtensionSet extensionSet,
                              QString dispName, std::function<void(RowId)> insertIdCallback, int hasIdType,
                              const bool isPrivate)
{
    QVector<RawDatabase::Query> queries;

    // qDebug() << QString("generateNewTextMessageQueries:hasIdType:") << hasIdType;

    queries += generateEnsurePkInChats(chatId);
    queries += generateEnsurePkInAuthors(sender);
    queries += generateUpdateAlias(sender, dispName);
    queries += generateHistoryTableInsertion('T', time, chatId);

    QVector<QByteArray> boundParams;
    QString queryString;

    if (hasIdType == 2) { // static_cast<int>(Widget::MessageHasIdType::NGC_MSG_ID)
        QString hexstr = message.section(':', 0, 0);
        QString message_real = message.section(':', 1);
        queryString = QStringLiteral(
                    "INSERT INTO text_messages (id, message_type, sender_alias, message, ngc_msgid, private) "
                    "VALUES ( "
                    "    last_insert_rowid(), "
                    "    'T', "
                    "    (SELECT id FROM aliases WHERE owner=");
        addAuthorIdSubQuery(queryString, boundParams, sender);
        queryString += " and display_name=?";
        boundParams += dispName.toUtf8();
        queryString += "), ?";
        boundParams += message_real.toUtf8();
        queryString += ", ?";
        boundParams += hexstr.toUtf8();
        if (isPrivate)
        {
            // qDebug() << QString("generateNewTextMessageQueries:isPrivate=true:") << isPrivate;
            queryString += ", '1'";
        }
        else
        {
            // qDebug() << QString("generateNewTextMessageQueries:isPrivate=false:") << isPrivate;
            queryString += ", '0'";
        }
        queryString += ");";

        // qDebug() << QString("generateNewTextMessageQueries:SQL:") << queryString;

    } else if (hasIdType == 3) { // static_cast<int>(Widget::MessageHasIdType::MSGV3_ID)
        QString hexstr = message.section(':', 0, 0);
        QString message_real = message.section(':', 1);
        queryString = QStringLiteral(
                    "INSERT INTO text_messages (id, message_type, sender_alias, message, msgv3hash) "
                    "VALUES ( "
                    "    last_insert_rowid(), "
                    "    'T', "
                    "    (SELECT id FROM aliases WHERE owner=");
        addAuthorIdSubQuery(queryString, boundParams, sender);
        queryString += " and display_name=?";
        boundParams += dispName.toUtf8();
        queryString += "), ?";
        boundParams += message_real.toUtf8();
        queryString += ", ?";
        boundParams += hexstr.toUtf8();
        queryString += ");";
    } else if (hasIdType == 1) { // static_cast<int>(Widget::MessageHasIdType::CONF_MSG_ID)
        QString hexstr = message.section(':', 0, 0);
        QString message_real = message.section(':', 1);
        queryString = QStringLiteral(
                    "INSERT INTO text_messages (id, message_type, sender_alias, message, conf_msgid) "
                    "VALUES ( "
                    "    last_insert_rowid(), "
                    "    'T', "
                    "    (SELECT id FROM aliases WHERE owner=");
        addAuthorIdSubQuery(queryString, boundParams, sender);
        queryString += " and display_name=?";
        boundParams += dispName.toUtf8();
        queryString += "), ?";
        boundParams += message_real.toUtf8();
        queryString += ", ?";
        boundParams += hexstr.toUtf8();
        queryString += ");";

    } else {
        queryString = QStringLiteral(
                    "INSERT INTO text_messages (id, message_type, sender_alias, message) "
                    "VALUES ( "
                    "    last_insert_rowid(), "
                    "    'T', "
                    "    (SELECT id FROM aliases WHERE owner=");
        addAuthorIdSubQuery(queryString, boundParams, sender);
        queryString += " and display_name=?";
        boundParams += dispName.toUtf8();
        queryString += "), ?";
        boundParams += message.toUtf8();
        queryString += ");";
    }

    queries += RawDatabase::Query(queryString, boundParams, insertIdCallback);

    if (!isDelivered) {
        queries += RawDatabase::Query{
            QString("INSERT INTO faux_offline_pending (id, required_extensions) VALUES ("
                    "    last_insert_rowid(), %1"
                    ");")
                .arg(extensionSet.to_ulong())};
    }

    return queries;
}

QVector<RawDatabase::Query> generateNewSystemMessageQueries(const ChatId& chatId,
                                                            const SystemMessage& systemMessage)
{
    QVector<RawDatabase::Query> queries;

    queries += generateEnsurePkInChats(chatId);
    queries += generateHistoryTableInsertion('S', systemMessage.timestamp, chatId);

    QVector<QByteArray> blobs;
    std::transform(systemMessage.args.begin(), systemMessage.args.end(), std::back_inserter(blobs),
                   [](const QString& s) { return s.toUtf8(); });

    queries += RawDatabase::Query(QString("INSERT INTO system_messages (id, message_type, "
                                          "system_message_type, arg1, arg2, arg3, arg4)"
                                          "VALUES (last_insert_rowid(), 'S', %1, ?, ?, ?, ?)")
                                      .arg(static_cast<int>(systemMessage.messageType)),
                                  blobs);

    return queries;
}
} // namespace

/**
 * @class History
 * @brief Interacts with the profile database to save the chat history.
 *
 * @var QHash<QString, int64_t> History::peers
 * @brief Maps friend public keys to unique IDs by index.
 * Caches mappings to speed up message saving.
 */

FileDbInsertionData::FileDbInsertionData()
{
    static int id = qRegisterMetaType<FileDbInsertionData>();
    (void)id;
}

/**
 * @brief Prepares the database to work with the history.
 * @param db This database will be prepared for use with the history.
 */
History::History(std::shared_ptr<RawDatabase> db_, Settings& settings_, IMessageBoxManager& messageBoxManager)
    : db(db_)
    , settings(settings_)
{
    if (!isValid()) {
        qWarning() << "Database not open, init failed";
        return;
    }

    // foreign key support is not enabled by default, so needs to be enabled on every connection
    // support was added in sqlite 3.6.19, which is qTox's minimum supported version
    db->execNow(
        "PRAGMA foreign_keys = ON;");

    const auto upgradeSucceeded = DbUpgrader::dbSchemaUpgrade(db, messageBoxManager);

    // dbSchemaUpgrade may have put us in an invalid state
    if (!upgradeSucceeded) {
        db.reset();
        return;
    }

    connect(this, &History::fileInserted, this, &History::onFileInserted);
}

History::~History()
{
    if (!isValid()) {
        return;
    }

    // We could have execLater requests pending with a lambda attached,
    // so clear the pending transactions first
    db->sync();
}

/**
 * @brief Checks if the database was opened successfully
 * @return True if database if opened, false otherwise.
 */
bool History::isValid()
{
    return db && db->isOpen();
}

/**
 * @brief Checks if a chat has history
 * @param chatId
 * @return True if it does, false otherwise.
 */
bool History::historyExists(const ChatId& chatId)
{
    if (historyAccessBlocked()) {
        return false;
    }

    return !getMessagesForChat(chatId, 0, 1).empty();
}

/**
 * @brief Erases all the chat history from the database.
 */
void History::eraseHistory()
{
    if (!isValid()) {
        return;
    }

    db->execNow("DELETE FROM faux_offline_pending;"
                "DELETE FROM broken_messages;"
                "DELETE FROM text_messages;"
                "DELETE FROM file_transfers;"
                "DELETE FROM system_messages;"
                "DELETE FROM history;"
                "DELETE FROM chats;"
                "DELETE FROM aliases;"
                "DELETE FROM authors;"
                "VACUUM;");
}

/**
 * @brief Erases the chat history of one chat.
 * @param chatId Chat ID to erase.
 */
void History::removeChatHistory(const ChatId& chatId)
{
    if (!isValid()) {
        return;
    }

    QVector<RawDatabase::Query> queries;
    QVector<QByteArray> boundParams;
    QString queryString = QStringLiteral(
        "DELETE FROM faux_offline_pending "
        "WHERE faux_offline_pending.id IN ( "
        "    SELECT faux_offline_pending.id FROM faux_offline_pending "
        "    LEFT JOIN history ON faux_offline_pending.id = history.id "
        "    WHERE chat_id=");
    addChatIdSubQuery(queryString, boundParams, chatId);
    queryString += QStringLiteral(")");
    queries += {queryString, boundParams};
    boundParams.clear();

    queryString = QStringLiteral(
        "DELETE FROM broken_messages "
        "WHERE broken_messages.id IN ( "
        "    SELECT broken_messages.id FROM broken_messages "
        "    LEFT JOIN history ON broken_messages.id = history.id "
        "    WHERE chat_id=");
    addChatIdSubQuery(queryString, boundParams, chatId);
    queryString += QStringLiteral(")");
    queries += {queryString, boundParams};
    boundParams.clear();

    queryString = QStringLiteral(
        "DELETE FROM text_messages "
        "WHERE id IN ("
        "   SELECT id from history "
        "   WHERE message_type = 'T' AND chat_id=");
    addChatIdSubQuery(queryString, boundParams, chatId);
    queryString += QStringLiteral(")");
    queries += {queryString, boundParams};
    boundParams.clear();

    queryString = QStringLiteral(
        "DELETE FROM file_transfers "
        "WHERE id IN ( "
        "    SELECT id from history "
        "    WHERE message_type = 'F' AND chat_id=");
    addChatIdSubQuery(queryString, boundParams, chatId);
    queryString += QStringLiteral(")");
    queries += {queryString, boundParams};
    boundParams.clear();

    queryString = QStringLiteral(
        "DELETE FROM system_messages "
        "WHERE id IN ( "
        "   SELECT id from history "
        "   WHERE message_type = 'S' AND chat_id=");
    addChatIdSubQuery(queryString, boundParams, chatId);
    queryString += QStringLiteral(")");
    queries += {queryString, boundParams};
    boundParams.clear();

    queryString = QStringLiteral(
        "DELETE FROM history WHERE chat_id=");
    addChatIdSubQuery(queryString, boundParams, chatId);
    queries += {queryString, boundParams};
    boundParams.clear();

    queryString = QStringLiteral(
        "DELETE FROM chats WHERE id=");
    addChatIdSubQuery(queryString, boundParams, chatId);
    queries += {queryString, boundParams};
    boundParams.clear();

    queries += RawDatabase::Query{QStringLiteral(
        "DELETE FROM aliases WHERE id NOT IN ( "
        "   SELECT DISTINCT sender_alias FROM text_messages "
        "   UNION "
        "   SELECT DISTINCT sender_alias FROM file_transfers)")};

    queries += RawDatabase::Query{QStringLiteral(
        "DELETE FROM authors WHERE id NOT IN ( "
        "   SELECT DISTINCT owner FROM aliases)")};

    if (!db->execNow(queries)) {
        qWarning() << "Failed to remove friend's history";
    } else {
        db->execNow(RawDatabase::Query{QStringLiteral("VACUUM")});
    }
}

void History::onFileInserted(RowId dbId, QByteArray fileId)
{
    auto& fileInfo = fileInfos[fileId];
    if (fileInfo.finished) {
        db->execLater(
            generateFileFinished(dbId, fileInfo.success, fileInfo.filePath, fileInfo.fileHash));
        fileInfos.remove(fileId);
    } else {
        fileInfo.finished = false;
        fileInfo.fileId = dbId;
    }
}

QVector<RawDatabase::Query>
History::generateNewFileTransferQueries(const ChatId& chatId, const ToxPk& sender,
                                        const QDateTime& time, const QString& dispName,
                                        const FileDbInsertionData& insertionData)
{
    QVector<RawDatabase::Query> queries;

    queries += generateEnsurePkInChats(chatId);
    queries += generateEnsurePkInAuthors(sender);
    queries += generateUpdateAlias(sender, dispName);
    queries += generateHistoryTableInsertion('F', time, chatId);

    std::weak_ptr<History> weakThis = shared_from_this();
    auto fileId = insertionData.fileId;

    QString queryString;
    queryString += QStringLiteral(
                               "INSERT INTO file_transfers "
                               "    (id, message_type, sender_alias, "
                               "    file_restart_id, file_name, file_path, "
                               "    file_hash, file_size, direction, file_state) "
                               "VALUES ( "
                               "    last_insert_rowid(), "
                               "    'F', "
                               "    (SELECT id FROM aliases WHERE owner=");
    QVector<QByteArray> boundParams;
    addAuthorIdSubQuery(queryString, boundParams, sender);
    queryString +=  " AND display_name=?";
    boundParams += dispName.toUtf8();
    queryString += "), ?";
    boundParams += insertionData.fileId;
    queryString += ", ?";
    boundParams += insertionData.fileName.toUtf8();
    queryString += ", ?";
    boundParams += insertionData.filePath.toUtf8();
    queryString += ", ?";
    boundParams += QByteArray();
    queryString += QStringLiteral(", %1, %2, %3);")
                        .arg(insertionData.size)
                        .arg(insertionData.direction)
                        .arg(ToxFile::CANCELED);
    queries += RawDatabase::Query(queryString, boundParams,
                           [weakThis, fileId](RowId id) {
                               auto pThis = weakThis.lock();
                               if (pThis)
                                   emit pThis->fileInserted(id, fileId);
                           });
    return queries;
}

RawDatabase::Query History::generateFileFinished(RowId id, bool success, const QString& filePath,
                                                 const QByteArray& fileHash)
{
    auto file_state = success ? ToxFile::FINISHED : ToxFile::CANCELED;
    if (filePath.length()) {
        return RawDatabase::Query(QStringLiteral("UPDATE file_transfers "
                                                 "SET file_state = %1, file_path = ?, file_hash = ?"
                                                 "WHERE id = %2")
                                      .arg(file_state)
                                      .arg(id.get()),
                                  {filePath.toUtf8(), fileHash});
    } else {
        return RawDatabase::Query(QStringLiteral("UPDATE file_transfers "
                                                 "SET file_state = %1 "
                                                 "WHERE id = %2")
                                      .arg(file_state)
                                      .arg(id.get()));
    }
}

void History::addNewFileMessage(const ChatId& chatId, const QByteArray& fileId,
                                const QString& fileName, const QString& filePath, int64_t size,
                                const ToxPk& sender, const QDateTime& time, QString const& dispName)
{
    if (historyAccessBlocked()) {
        return;
    }

    // This is an incredibly far from an optimal way of implementing this,
    // but given the frequency that people are going to be initiating a file
    // transfer we can probably live with it.

    // Since both inserting an alias for a user and inserting a file transfer
    // will generate new ids, there is no good way to inject both new ids into the
    // history query without refactoring our RawDatabase::Query and processor loops.

    // What we will do instead is chain callbacks to try to get reasonable behavior.
    // We can call the generateNewMessageQueries() fn to insert a message with an empty
    // message in it, and get the id with the callbck. Once we have the id we can ammend
    // the data to have our newly inserted file_id as well

    ToxFile::FileDirection direction;
    if (sender == chatId) {
        direction = ToxFile::RECEIVING;
    } else {
        direction = ToxFile::SENDING;
    }

    std::weak_ptr<History> weakThis = shared_from_this();
    FileDbInsertionData insertionData;
    insertionData.fileId = fileId;
    insertionData.fileName = fileName;
    insertionData.filePath = filePath;
    insertionData.size = size;
    insertionData.direction = direction;

    auto queries = generateNewFileTransferQueries(chatId, sender, time, dispName, insertionData);

    db->execLater(queries);
}

void History::addNewSystemMessage(const ChatId& chatId, const SystemMessage& systemMessage)
{
    if (historyAccessBlocked())
        return;

    const auto queries = generateNewSystemMessageQueries(chatId, systemMessage);

    db->execLater(queries);
}

/**
 * @brief Saves a chat message in the database.
 * @param chatId Chat ID to save.
 * @param message Message to save.
 * @param sender Sender to save.
 * @param time Time of message sending.
 * @param isDelivered True if message was already delivered.
 * @param dispName Name, which should be displayed.
 * @param insertIdCallback Function, called after query execution.
 */
void History::addNewMessage(const ChatId& chatId, const QString& message, const ToxPk& sender,
                            const QDateTime& time, bool isDelivered, ExtensionSet extensionSet,
                            QString dispName, const std::function<void(RowId)>& insertIdCallback,
                            const int hasIdType, const bool isPrivate)
{
    if (historyAccessBlocked()) {
        return;
    }

    // qDebug() << "History::addNewMessage: isPrivate:" << isPrivate;

    db->execLater(generateNewTextMessageQueries(chatId, message, sender, time, isDelivered,
                                                extensionSet, dispName, insertIdCallback, hasIdType, isPrivate));
}

static size_t xnet_pack_u16_hist(uint8_t *bytes, uint16_t v)
{
    bytes[0] = (v >> 8) & 0xff;
    bytes[1] = v & 0xff;
    return sizeof(v);
}

static size_t xnet_pack_u32_hist(uint8_t *bytes, uint32_t v)
{
    uint8_t *p = bytes;
    p += xnet_pack_u16_hist(p, (v >> 16) & 0xffff);
    p += xnet_pack_u16_hist(p, v & 0xffff);
    return p - bytes;
}

QList<History::HistMessage> History::getGroupMessagesXMinutesBack(const QByteArray& chatIdByteArray, const QDateTime& date,
                    const ToxPk& sender, int groupnumber, int peernumber)
{
    if (historyAccessBlocked()) {
        return {};
    }

    std::ignore = sender;

    QString queryText = QString("SELECT history.id, history.message_type, history.timestamp, "
                                "text_messages.message, "
                                "authors.public_key as sender_key, aliases.display_name, text_messages.ngc_msgid, "
                                "text_messages.msgv3hash, chats.uuid "
                                "FROM history "
                                "LEFT JOIN text_messages ON history.id = text_messages.id "
                                "JOIN chats ON chat_id = chats.id "
                                "LEFT JOIN aliases ON text_messages.sender_alias = aliases.id "
                                "LEFT JOIN authors ON aliases.owner = authors.id "
                                "WHERE history.chat_id = ");
    QVector<QByteArray> boundParams;
    boundParams.append(chatIdByteArray);
    queryText += "(SELECT id FROM chats WHERE uuid = ?)";
    queryText += QString(" AND timestamp >= %1").arg(date.toMSecsSinceEpoch());
    queryText += QString(" AND text_messages.private = '0'");
    queryText += QString(" order by timestamp ASC;");

    // qDebug() << QString("getGroupMessagesXMinutesBack:SQL:") << queryText;
    // const bool isGuiThread2 = QThread::currentThread() == QCoreApplication::instance()->thread();
    // qDebug() << QString("getGroupMessagesXMinutesBack:THREAD:001:") << QThread::currentThreadId() << "isGuiThread" << isGuiThread2;

    Tox* toxcore = settings.getToxcore();
    QList<HistMessage> messages;
    auto rowCallback = [&toxcore, groupnumber, peernumber](const QVector<QVariant>& row) {
        auto it = row.begin();

        constexpr auto messageOffset = 3;
        constexpr auto senderOffset = 4;

        const auto id = RowId{(*it++).toLongLong()};
        std::ignore = id;
        const auto messageType = (*it++).toString();
        const auto timestamp = QDateTime::fromMSecsSinceEpoch((*it++).toLongLong());

        assert(messageType.size() == 1);
        switch (messageType[0].toLatin1()) {
        case 'T': {
            it = std::next(row.begin(), messageOffset);
            assert(!it->isNull());
            auto messageContent = (*it++).toString();

            it = std::next(row.begin(), senderOffset);
            // ---------------------
            const auto senderKey__bin = (*it++).toByteArray();
            const auto senderKey__str = QString::fromUtf8(senderKey__bin.toHex()).toUpper();
            // ---------------------
            const auto senderName__bin = (*it).toByteArray();
            auto senderName__str = QString::fromUtf8((*it++).toByteArray().replace('\0', ""));
            senderName__str = senderName__str.section(':', 1);
            // ---------------------
            const auto ngc_msgid2__bin = (*it).toByteArray();
            const auto ngc_msgid2__str = QString::fromUtf8((*it++).toByteArray().replace('\0', ""));
            // ---------------------
            const auto msgv3hash = QString::fromUtf8((*it++).toByteArray().replace('\0', ""));
            // ---------------------
            const auto chat_uuid__bin = (*it++).toByteArray();
            const auto chat_uuid__str = QString::fromUtf8(chat_uuid__bin.toHex()).toUpper();
            // ---------------------

            // qDebug() << QString("getGroupMessagesXMinutesBack:M:")
            //     << "timestamp" << timestamp
            //     << "chat_uuid" << chat_uuid__str
            //     << "senderKey" << senderKey__str
            //     << "messageContent" << messageContent
            //     << "senderName" << senderName__str
            //     << "ngc_msgid2" << ngc_msgid2__str.left(16)
            //     << "msgv3hash" << msgv3hash.left(16)
            //     ;

            if ((messageContent == "___") && (ngc_msgid2__str.size() > 8))
            {
                // HINT: message is a group image
                // qDebug() << QString("getGroupMessagesXMinutesBack:M:__group_image");
            }
            else
            {
                if (messageContent.size() == 0)
                {
                    // qDebug() << QString("getGroupMessagesXMinutesBack: messageContent has zero size");
                    break;
                }
                // HINT: regular group text message
                // qDebug() << QString("getGroupMessagesXMinutesBack:M:__group_text message");

                const int delay = 301;
#if QT_VERSION < QT_VERSION_CHECK( 5, 10, 0 )
                int rndi = qrand() % delay;
#else
                int rndi = QRandomGenerator::global()->generate() % delay;
#endif
                int n = 300 + rndi;
                std::ignore = n;

                const int header_length = 6 + 1 + 1 + 4 + 32 + 4 + 25;
                const int data_length = header_length + messageContent.toUtf8().size();

                if (data_length < (header_length + 1) || (data_length > 40000))
                {
                    qDebug() << QString("getGroupMessagesXMinutesBack: some error in calculating data length");
                    break;
                }

                uint8_t* data_buf = reinterpret_cast<uint8_t*>(calloc(1, data_length));
                if (data_buf)
                {
                    // -----------------------------------------------------------
                    // header (8 bytes)
                    uint8_t* data_buf_cur = data_buf;
                    *data_buf_cur = 0x66;
                    data_buf_cur++;
                    *data_buf_cur = 0x77;
                    data_buf_cur++;
                    *data_buf_cur = 0x88;
                    data_buf_cur++;
                    *data_buf_cur = 0x11;
                    data_buf_cur++;
                    *data_buf_cur = 0x34;
                    data_buf_cur++;
                    *data_buf_cur = 0x35;
                    data_buf_cur++;
                    *data_buf_cur = 0x1;
                    data_buf_cur++;
                    *data_buf_cur = 0x2;
                    data_buf_cur++;
                    // -----------------------------------------------------------
                    // ngc message id (4 bytes)
                    const auto ngc_msgid2__bin_bytes = QByteArray::fromHex(ngc_msgid2__str.toLatin1());
                    if (ngc_msgid2__bin_bytes.size() != 4)
                    {
                        qDebug() << QString("getGroupMessagesXMinutesBack: ngc_msgid2__bin_bytes.size() != 4") << ngc_msgid2__bin_bytes.size();
                        free(data_buf);
                        break;
                    }
                    memcpy(data_buf_cur, reinterpret_cast<const uint8_t*>(ngc_msgid2__bin_bytes.constData()), 4);
                    data_buf_cur = data_buf_cur + 4;
                    // -----------------------------------------------------------
                    // sender peer pubkey (32 bytes)
                    const auto senderKey__bin_bytes = QByteArray::fromHex(senderKey__str.toLatin1());
                    if (senderKey__bin_bytes.size() != 32)
                    {
                        qDebug() << QString("getGroupMessagesXMinutesBack: senderKey__bin_bytes.size() != 32") << senderKey__bin_bytes.size();
                        free(data_buf);
                        break;
                    }
                    memcpy(data_buf_cur, reinterpret_cast<const uint8_t*>(senderKey__bin_bytes.constData()), 32);
                    data_buf_cur = data_buf_cur + 32;
                    // -----------------------------------------------------------
                    // timestamp // (unix timestamp 4 bytes)
                    uint32_t timestamp_c = static_cast<uint32_t>(timestamp.toMSecsSinceEpoch() / 1000);
                    uint32_t timestamp_unix_buf;
                    xnet_pack_u32_hist(reinterpret_cast<uint8_t*>(&timestamp_unix_buf), timestamp_c);
                    memcpy(data_buf_cur, &timestamp_unix_buf, 4);
                    data_buf_cur = data_buf_cur + 4;
                    // -----------------------------------------------------------
                    // sender name (cut to 25 bytes)
                    uint8_t* name_buf = reinterpret_cast<uint8_t*>(calloc(1, 25));
                    if (name_buf)
                    {
                        const auto senderName__bin_bytes = senderName__str.toUtf8();
                        const int name_in_bytes = senderName__bin_bytes.size();
                        // qDebug() << QString("getGroupMessagesXMinutesBack: senderName__bin_bytes.size():")
                        //     << senderName__bin_bytes.size();

                        // qDebug() << QString("getGroupMessagesXMinutesBack:senderName__bin_bytes hex:")
                        //     << QString::fromUtf8(senderName__bin_bytes.toHex()).toUpper();

                        const int max_name_bytes = 25;
                        if (name_in_bytes > max_name_bytes)
                        {
                            memcpy(name_buf, reinterpret_cast<const uint8_t*>(senderName__bin_bytes.constData()), max_name_bytes);
                        }
                        else
                        {
                            memcpy(name_buf, reinterpret_cast<const uint8_t*>(senderName__bin_bytes.constData()), name_in_bytes);
                        }
                        memcpy(data_buf_cur, reinterpret_cast<const uint8_t*>(name_buf), max_name_bytes);
                        free(name_buf);
                    }
                    data_buf_cur = data_buf_cur + 25;
                    // -----------------------------------------------------------
                    // the actual message text
                    memcpy(data_buf_cur, reinterpret_cast<const uint8_t*>(messageContent.toUtf8().constData()),
                                messageContent.toUtf8().size());
                    // -----------------------------------------------------------
                    // now send the whole thing
                    //
                    // QByteArray data_buf_bytearray = QByteArray(reinterpret_cast<const char*>(data_buf), data_length);
                    // qDebug() << QString("getGroupMessagesXMinutesBack:send_bytes:") << QString::fromUtf8(data_buf_bytearray.toHex()).toUpper();
                    if (toxcore != nullptr)
                    {
                        const bool isGuiThread3 = QThread::currentThread() == QCoreApplication::instance()->thread();
                        qDebug() << QString("getGroupMessagesXMinutesBack:THREAD:002:") << QThread::currentThreadId() << "isGuiThread" << isGuiThread3;
                        // HINT: wait "n" milliseconds before sending the sync message
                        QThread::msleep(n);
                        Tox_Err_Group_Send_Custom_Private_Packet error;
                        std::ignore = error;
                        int result = tox_group_send_custom_private_packet(toxcore, (groupnumber - Settings::NGC_GROUPNUM_OFFSET),
                                                                          peernumber, 1, data_buf,
                                                                          data_length, &error);
                        std::ignore = result;
                        // qDebug() << QString("getGroupMessagesXMinutesBack:sending_request:groupnumber:") << groupnumber << "groupnumber_corr:" << (groupnumber - Settings::NGC_GROUPNUM_OFFSET);
                        // qDebug() << QString("getGroupMessagesXMinutesBack:sending_request:result:") << result << "error:" << error;
                    }
                    free(data_buf);
                }
                else
                {
                    qDebug() << QString("getGroupMessagesXMinutesBack: error allocating buffer");
                }
            }

            break;
            }
        }
    };

    db->execNow({queryText, boundParams, rowCallback});

    return messages;
}

void History::addPushtoken(const ToxPk& sender, const QString& pushtoken)
{
    if (!isValid()) {
        return;
    }

    db->execNow(
               RawDatabase::Query(QStringLiteral("UPDATE authors "
                                                 "set push_token = ? "
                                                 "WHERE public_key = ?"),
                            {pushtoken.toUtf8(), sender.getByteArray()})
               );
}

QString History::getPushtoken(const ToxPk& friendPk)
{
    if (!isValid()) {
        return QString("_");
    }

    QString pushtoken = QString("_");
    db->execNow(
        RawDatabase::Query("SELECT push_token from authors WHERE public_key = ?",
            {friendPk.getByteArray()},
            [&](const QVector<QVariant>& row) {
                    pushtoken = row[0].toString();
                    qDebug() << "getPushtoken:" << pushtoken;
            })
    );

    return pushtoken;
}

QString History::getSqlcipherVersion()
{
    if (!isValid()) {
        return QString("");
    }

    QString sqlcipherVersion = QString("");
    db->execNow(
        RawDatabase::Query("PRAGMA cipher_version;",
            [&](const QVector<QVariant>& row) {
                    sqlcipherVersion = row[0].toString();
                    qDebug() << "getSqlcipherVersion:" << sqlcipherVersion;
            })
    );

    return sqlcipherVersion;
}

void History::pushtokenPing(const ToxPk& sender)
{
    if (!isValid()) {
        return;
    }

    if (!settings.getUsePushNotification()) {
        qDebug() << "pushtokenPing:UsePushNotification set to false. Push Notification NOT sent!";
        return;
    } else {
        db->execNow(
            RawDatabase::Query("SELECT push_token from authors WHERE public_key = ?",
                {sender.getByteArray()},
                [&](const QVector<QVariant>& row) {
                        auto url = row[0].toString();

                        qDebug() << "wakeupMobile:START";
                        qDebug() << "pushtokenPing:pushtoken=" << url;
                        qDebug() << "pushtokenPing:pushtoken(as hex bytes)=" << url.toLatin1().toHex();

                        if (url.isNull()) {
                            qDebug() << "pushtokenPing:url.isNull()";
                        }
                        else if (url.size() < 8) {
                            qDebug() << "pushtokenPing:url.size() < 8";
                        }
                        else if (url.isEmpty()) {
                            qDebug() << "pushtokenPing:url.isEmpty()";
                        }
                        else if (url.startsWith("https://")) {

                            bool push_url_in_whitelist = false;
                            foreach (QString listitem, Settings::PUSHURL_WHITELIST) {
                                qDebug() << "wakeupMobile:check against whitelist: " << listitem << " -> " << url;
                                if (url.startsWith(listitem)) {
                                    push_url_in_whitelist = true;
                                    break;
                                }
                            }

                            if (push_url_in_whitelist) {

                                qDebug() << "wakeupMobile:push_url=" << url;
#if 1
                                qDebug() << "wakeupMobile:network method:QNetworkAccessManager";

                                QString proxy_addr = settings.getProxyAddr();
                                quint16 proxy_port = settings.getProxyPort();
                                QNetworkProxy proxy = settings.getProxy();
                                ICoreSettings::ProxyType proxy_type = settings.getProxyType();
                                qDebug() << "wakeupMobile:proxy_addr=" << proxy_addr.toUtf8() << " bytes=" << proxy_addr.toUtf8().size();
                                qDebug() << "wakeupMobile:proxy_port=" << proxy_port;
                                qDebug() << "wakeupMobile:proxy_type=" << static_cast<int>(proxy_type);
                                qDebug() << "wakeupMobile:proxy=" << proxy;

                                QNetworkAccessManager *nam = new QNetworkAccessManager(QThread::currentThread());
                                nam->setProxy(proxy);

                                QUrlQuery paramsQuery;
                                paramsQuery.addQueryItem("ping", "1");

                                QUrl resource(url);

                                QNetworkRequest request(resource);
                                request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
                                request.setRawHeader("User-Agent", "Mozilla/5.0 (Windows NT 6.1; rv:60.0) Gecko/20100101 Firefox/60.0");

                                connect(nam, &QNetworkAccessManager::finished,[=](QNetworkReply* reply){
                                    if (reply->error() == QNetworkReply::NoError) {
                                        qDebug() << "pushPingFinished:OK";
                                    } else {
                                        qDebug() << "pushPingFinished:error:" << reply->errorString();
                                    }
                                });

                                qDebug() << "wakeupMobile:calling url ...";
                                nam->post(request, paramsQuery.query(QUrl::FullyEncoded).toUtf8());
#else

                                qDebug() << "wakeupMobile:network method:CURL";
                                curl_global_init(CURL_GLOBAL_ALL);
                                CURL *curl;
                                CURLcode res;
                                curl = curl_easy_init();

                                // HINT: show verbose curl output
                                curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

                                if (curl)
                                {
                                    const char *url_c_str = url.toLatin1().constData();
#if defined(Q_OS_WIN32)
                                    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
#endif

                                    QString proxy_addr = settings.getProxyAddr();
                                    quint16 proxy_port = settings.getProxyPort();
                                    QNetworkProxy proxy = settings.getProxy();
                                    ICoreSettings::ProxyType proxy_type = settings.getProxyType();
                                    qDebug() << "wakeupMobile:proxy_addr=" << proxy_addr.toUtf8() << " bytes=" << proxy_addr.toUtf8().size();
                                    qDebug() << "wakeupMobile:proxy_port=" << proxy_port;
                                    qDebug() << "wakeupMobile:proxy_type=" << static_cast<int>(proxy_type);
                                    qDebug() << "wakeupMobile:proxy=" << proxy;

                                    if (proxy_type != ICoreSettings::ProxyType::ptNone) {
                                        if (static_cast<uint32_t>(proxy_addr.length()) > 300) {
                                            qWarning() << "Proxy address" << proxy_addr << "is too long (max. 300 chars)";
                                        } else if (!proxy_addr.isEmpty() && proxy_port > 0) {
                                            if (proxy_type == ICoreSettings::ProxyType::ptSOCKS5) {
                                                curl_easy_setopt(curl, CURLOPT_PROXY, proxy_addr.toUtf8().data());
                                                curl_easy_setopt(curl, CURLOPT_PROXYPORT, proxy_port);
                                                curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5);
                                                curl_easy_setopt(curl, CURLOPT_HTTPPROXYTUNNEL, 1);
                                                qDebug() << "Using CURLPROXY_SOCKS5 proxy" << proxy_addr << ":" << proxy_port;
                                            } else if (proxy_type == ICoreSettings::ProxyType::ptHTTP) {
                                                curl_easy_setopt(curl, CURLOPT_PROXY, proxy_addr.toUtf8().data());
                                                curl_easy_setopt(curl, CURLOPT_PROXYPORT, proxy_port);
                                                curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
                                                curl_easy_setopt(curl, CURLOPT_HTTPPROXYTUNNEL, 1);
                                                qDebug() << "Using CURLPROXY_HTTP proxy" << proxy_addr << ":" << proxy_port;
                                            }
                                        }
                                    }

                                    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "ping=1");
                                    curl_easy_setopt(curl, CURLOPT_URL, url_c_str);
                                    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 6.1; rv:60.0) Gecko/20100101 Firefox/60.0");
                                    res = curl_easy_perform(curl);

                                    if (res == CURLE_OK)
                                    {
                                        qDebug() << "curl:CURLE_OK";
                                    }
                                    else
                                    {
                                        qDebug() << "curl:ERROR:res=" << res;
                                    }

                                    curl_easy_cleanup(curl);
                                }
                                curl_global_cleanup();
#endif
                            } else {
                                qDebug() << "wakeupMobile:check against whitelist:URL not in whitelist -> " << url;
                            }
                        } else {
                            qDebug() << "pushtokenPing:some other problem";
                        }
                })
            );
    }
}

void History::setFileFinished(const QByteArray& fileId, bool success, const QString& filePath,
                              const QByteArray& fileHash)
{
    if (historyAccessBlocked()) {
        return;
    }

    auto& fileInfo = fileInfos[fileId];
    if (fileInfo.fileId.get() == -1) {
        fileInfo.finished = true;
        fileInfo.success = success;
        fileInfo.filePath = filePath;
        fileInfo.fileHash = fileHash;
    } else {
        db->execLater(generateFileFinished(fileInfo.fileId, success, filePath, fileHash));
    }

    fileInfos.remove(fileId);
}

size_t History::getNumMessagesForChat(const ChatId& chatId)
{
    if (historyAccessBlocked()) {
        return 0;
    }

    return getNumMessagesForChatBeforeDate(chatId, QDateTime());
}

size_t History::getNumMessagesForChatBeforeDate(const ChatId& chatId, const QDateTime& date)
{
    if (historyAccessBlocked()) {
        return 0;
    }

    QString queryText = QString("SELECT COUNT(history.id) "
                                "FROM history "
                                "JOIN chats ON chat_id = chats.id "
                                "WHERE chats.uuid = ?");

    if (date.isNull()) {
        queryText += ";";
    } else {
        queryText += QString(" AND timestamp < %1;").arg(date.toMSecsSinceEpoch());
    }

    size_t numMessages = 0;
    auto rowCallback = [&numMessages](const QVector<QVariant>& row) {
        numMessages = row[0].toLongLong();
    };

    db->execNow({queryText, {chatId.getByteArray()}, rowCallback});

    return numMessages;
}

QList<History::HistMessage> History::getMessagesForChat(const ChatId& chatId, size_t firstIdx,
                                                          size_t lastIdx)
{
    if (historyAccessBlocked()) {
        return {};
    }

    QList<HistMessage> messages;

    auto rowCallback = [&chatId, &messages](const QVector<QVariant>& row) {
        // If the select statement is changed please update these constants
        constexpr auto messageOffset = 6;
        constexpr auto fileOffset = 7;
        constexpr auto senderOffset = 13;
        constexpr auto systemOffset = 16;

        auto it = row.begin();

        const auto id = RowId{(*it++).toLongLong()};
        const auto messageType = (*it++).toString();
        const auto timestamp = QDateTime::fromMSecsSinceEpoch((*it++).toLongLong());
        const auto isPending = !(*it++).isNull();
        // If NULL this should just reutrn 0 which is an empty extension set, good enough for now
        const auto requiredExtensions = ExtensionSet((*it++).toLongLong());
        const auto isBroken = !(*it++).isNull();
        const auto messageState = getMessageState(isPending, isBroken);

        // Intentionally arrange query so message types are at the end so we don't have to think
        // about the iterator jumping around after handling the different types.
        assert(messageType.size() == 1);
        switch (messageType[0].toLatin1()) {
        case 'T': {
            it = std::next(row.begin(), messageOffset);
            assert(!it->isNull());
            auto messageContent = (*it++).toString();
            it = std::next(row.begin(), senderOffset);
            const auto senderKey = ToxPk{(*it++).toByteArray()};
            const auto senderName = QString::fromUtf8((*it++).toByteArray().replace('\0', ""));
            const auto ngc_msgid2 = QString::fromUtf8((*it++).toByteArray().replace('\0', ""));
            // qDebug() << QString("getMessagesForChat:ngc_msgid:") << ngc_msgid2.left(16);
            if (messageContent.size() == 0)
            {
                messageContent = "___";
            }
            HistMessage h = HistMessage(id, messageState, requiredExtensions, timestamp,
                            chatId.clone(), senderName, senderKey, messageContent, ngc_msgid2);
            // qDebug() << QString("getMessagesForChat:ngcMsgid:") << h.ngcMsgid.left(16);
            messages += h;
            break;
        }
        case 'F': {
            it = std::next(row.begin(), fileOffset);
            assert(!it->isNull());
            const auto fileKind = TOX_FILE_KIND_DATA;
            const auto resumeFileId = (*it++).toByteArray();
            const auto fileName = (*it++).toString();
            const auto filePath = (*it++).toString();
            const auto filesize = (*it++).toLongLong();
            const auto direction = static_cast<ToxFile::FileDirection>((*it++).toLongLong());
            const auto status = static_cast<ToxFile::FileStatus>((*it++).toLongLong());

            ToxFile file(0, 0, fileName, filePath, filesize, direction, fileKind);
            file.resumeFileId = resumeFileId;
            file.status = status;

            it = std::next(row.begin(), senderOffset);
            const auto senderKey = ToxPk{(*it++).toByteArray()};
            const auto senderName = QString::fromUtf8((*it++).toByteArray().replace('\0', ""));
            messages += HistMessage(id, messageState, timestamp, chatId.clone(), senderName,
                                    senderKey, file);
            break;
        }
        default:
        case 'S':
            it = std::next(row.begin(), systemOffset);
            assert(!it->isNull());
            SystemMessage systemMessage;
            systemMessage.messageType = static_cast<SystemMessageType>((*it++).toLongLong());
            systemMessage.timestamp = timestamp;

            auto argEnd = std::next(it, systemMessage.args.size());
            std::transform(it, argEnd, systemMessage.args.begin(), [](const QVariant& arg) {
                return QString::fromUtf8(arg.toByteArray().replace('\0', ""));
            });
            it = argEnd;

            messages += HistMessage(id, timestamp, chatId.clone(), systemMessage);
            break;
        }
    };

    // Don't forget to update the rowCallback if you change the selected columns!
    QString queryString =
        QStringLiteral(
            "SELECT history.id, history.message_type, history.timestamp, faux_offline_pending.id, "
            "    faux_offline_pending.required_extensions, broken_messages.id, text_messages.message, "
            "    file_restart_id, file_name, file_path, file_size, file_transfers.direction, "
            "    file_state, authors.public_key as sender_key, aliases.display_name, text_messages.ngc_msgid, "
            "    system_messages.system_message_type, system_messages.arg1, system_messages.arg2, "
            "    system_messages.arg3, system_messages.arg4 "
            "FROM history "
            "LEFT JOIN text_messages ON history.id = text_messages.id "
            "LEFT JOIN file_transfers ON history.id = file_transfers.id "
            "LEFT JOIN system_messages ON system_messages.id == history.id "
            "LEFT JOIN aliases ON text_messages.sender_alias = aliases.id OR "
            "file_transfers.sender_alias = aliases.id "
            "LEFT JOIN authors ON aliases.owner = authors.id "
            "LEFT JOIN faux_offline_pending ON faux_offline_pending.id = history.id "
            "LEFT JOIN broken_messages ON broken_messages.id = history.id "
            "WHERE history.chat_id = ");
    QVector<QByteArray> boundParams;
    addChatIdSubQuery(queryString, boundParams, chatId);
    queryString += QStringLiteral(
            " LIMIT %1 OFFSET %2;")
            .arg(lastIdx - firstIdx)
            .arg(firstIdx);
    db->execNow({queryString, boundParams, rowCallback});

    return messages;
}

QList<History::HistMessage> History::getUndeliveredMessagesForChat(const ChatId& chatId)
{
    if (historyAccessBlocked()) {
        return {};
    }

    QList<History::HistMessage> ret;
    auto rowCallback = [&chatId, &ret](const QVector<QVariant>& row) {
        auto it = row.begin();
        // dispName and message could have null bytes, QString::fromUtf8
        // truncates on null bytes so we strip them
        auto id = RowId{(*it++).toLongLong()};
        auto timestamp = QDateTime::fromMSecsSinceEpoch((*it++).toLongLong());
        auto isPending = !(*it++).isNull();
        auto extensionSet = ExtensionSet((*it++).toLongLong());
        auto isBroken = !(*it++).isNull();
        auto messageContent = (*it++).toString();
        auto senderKey = ToxPk{(*it++).toByteArray()};
        auto displayName = QString::fromUtf8((*it++).toByteArray().replace('\0', ""));
        auto ngc_msgid3 = QString::fromUtf8((*it++).toByteArray().replace('\0', ""));

        MessageState messageState = getMessageState(isPending, isBroken);

        ret += {id,          messageState, extensionSet,  timestamp, chatId.clone(),
                displayName, senderKey,    messageContent, ngc_msgid3};
    };

    QString queryString =
        QStringLiteral(
            "SELECT history.id, history.timestamp, faux_offline_pending.id, "
            "    faux_offline_pending.required_extensions, broken_messages.id, text_messages.message, "
            "    authors.public_key as sender_key, aliases.display_name, text_messages.ngc_msgid "
            "FROM history "
            "JOIN text_messages ON history.id = text_messages.id "
            "JOIN aliases ON text_messages.sender_alias = aliases.id "
            "JOIN authors ON aliases.owner = authors.id "
            "JOIN faux_offline_pending ON faux_offline_pending.id = history.id "
            "LEFT JOIN broken_messages ON broken_messages.id = history.id "
            "WHERE history.chat_id = ");
    QVector<QByteArray> boundParams;
    addChatIdSubQuery(queryString, boundParams, chatId);
    queryString += QStringLiteral(" AND history.message_type = 'T';");
    db->execNow({queryString, boundParams, rowCallback});

    return ret;
}

/**
 * @brief Search phrase in chat messages
 * @param chatId Chat ID
 * @param from a date message where need to start a search
 * @param phrase what need to find
 * @param parameter for search
 * @return date of the message where the phrase was found
 */
QDateTime History::getDateWhereFindPhrase(const ChatId& chatId, const QDateTime& from,
                                          QString phrase, const ParameterSearch& parameter)
{
    if (historyAccessBlocked()) {
        return QDateTime();
    }

    QDateTime result;
    auto rowCallback = [&result](const QVector<QVariant>& row) {
        result = QDateTime::fromMSecsSinceEpoch(row[0].toLongLong());
    };

    phrase.replace("'", "''");

    QString message;

    switch (parameter.filter) {
    case FilterSearch::Register:
        message = QStringLiteral("text_messages.message LIKE '%%1%'").arg(phrase);
        break;
    case FilterSearch::WordsOnly:
        message = QStringLiteral("text_messages.message REGEXP '%1'")
                      .arg(SearchExtraFunctions::generateFilterWordsOnly(phrase).toLower());
        break;
    case FilterSearch::RegisterAndWordsOnly:
        message = QStringLiteral("REGEXPSENSITIVE(text_messages.message, '%1')")
                      .arg(SearchExtraFunctions::generateFilterWordsOnly(phrase));
        break;
    case FilterSearch::Regular:
        message = QStringLiteral("text_messages.message REGEXP '%1'").arg(phrase);
        break;
    case FilterSearch::RegisterAndRegular:
        message = QStringLiteral("REGEXPSENSITIVE(text_messages.message '%1')").arg(phrase);
        break;
    default:
        message = QStringLiteral("LOWER(text_messages.message) LIKE '%%1%'").arg(phrase.toLower());
        break;
    }

    QDateTime date = from;

    if (!date.isValid()) {
        date = QDateTime::currentDateTime();
    }

    if (parameter.period == PeriodSearch::AfterDate || parameter.period == PeriodSearch::BeforeDate) {

#if (QT_VERSION >= QT_VERSION_CHECK(5, 15, 0))
        date = parameter.date.startOfDay();
#else
        date = QDateTime(parameter.date);
#endif
    }

    QString period;
    switch (parameter.period) {
    case PeriodSearch::WithTheFirst:
        period = QStringLiteral("ORDER BY timestamp ASC LIMIT 1;");
        break;
    case PeriodSearch::AfterDate:
        period = QStringLiteral("AND timestamp > '%1' ORDER BY timestamp ASC LIMIT 1;")
                     .arg(date.toMSecsSinceEpoch());
        break;
    case PeriodSearch::BeforeDate:
        period = QStringLiteral("AND timestamp < '%1' ORDER BY timestamp DESC LIMIT 1;")
                     .arg(date.toMSecsSinceEpoch());
        break;
    default:
        period = QStringLiteral("AND timestamp < '%1' ORDER BY timestamp DESC LIMIT 1;")
                     .arg(date.toMSecsSinceEpoch());
        break;
    }

    auto query = RawDatabase::Query(
        QStringLiteral("SELECT timestamp "
                       "FROM history "
                       "JOIN chats ON chat_id = chats.id "
                       "JOIN text_messages ON history.id = text_messages.id "
                       "WHERE chats.uuid = ? "
                       "AND %1 "
                       "%2")
            .arg(message)
            .arg(period),
            {chatId.getByteArray()},
            rowCallback);

    db->execNow(query);

    return result;
}

/**
 * @brief Gets date boundaries in conversation with friendPk. History doesn't model conversation indexes,
 * but we can count messages between us and friendPk to effectively give us an index. This function
 * returns how many messages have happened between us <-> friendPk each time the date changes
 * @param[in] chatId ChatId of conversation to retrieve
 * @param[in] from Start date to look from
 * @param[in] maxNum Maximum number of date boundaries to retrieve
 * @note This API may seem a little strange, why not use QDate from and QDate to? The intent is to
 * have an API that can be used to get the first item after a date (for search) and to get a list
 * of date changes (for loadHistory). We could write two separate queries but the query is fairly
 * intricate compared to our other ones so reducing duplication of it is preferable.
 */
QList<History::DateIdx> History::getNumMessagesForChatBeforeDateBoundaries(const ChatId& chatId,
                                                                             const QDate& from,
                                                                             size_t maxNum)
{
    if (historyAccessBlocked()) {
        return {};
    }

    QList<DateIdx> dateIdxs;
    auto rowCallback = [&dateIdxs](const QVector<QVariant>& row) {
        DateIdx dateIdx;
        dateIdx.numMessagesIn = row[0].toLongLong();
        dateIdx.date =
            QDateTime::fromMSecsSinceEpoch(row[1].toLongLong() * 24 * 60 * 60 * 1000).date();
        dateIdxs.append(dateIdx);
    };

    // No guarantee that this is the most efficient way to do this...
    // We want to count messages that happened for a friend before a
    // certain date. We do this by re-joining our table a second time
    // but this time with the only filter being that our id is less than
    // the ID of the corresponding row in the table that is grouped by day
    auto countMessagesForFriend =
        QStringLiteral("SELECT COUNT(*) - 1 " // Count - 1 corresponds to 0 indexed message id for friend
                "FROM history countHistory "            // Import unfiltered table as countHistory
                "JOIN chats ON chat_id = chats.id " // link chat_id to chat.id
                "WHERE chats.uuid = ?"          // filter this conversation
                "AND countHistory.id <= history.id"); // and filter that our unfiltered table history id only has elements up to history.id

    auto limitString = (maxNum) ? QString("LIMIT %1").arg(maxNum) : QString("");

    auto query = RawDatabase::Query(QStringLiteral(
                        "SELECT (%1), (timestamp / 1000 / 60 / 60 / 24) AS day "
                               "FROM history "
                               "JOIN chats ON chat_id = chats.id "
                               "WHERE chats.uuid = ? "
                               "AND timestamp >= %2 "
                               "GROUP by day "
                               "%3;")
                           .arg(countMessagesForFriend)
#if (QT_VERSION >= QT_VERSION_CHECK(5, 15, 0))
                           .arg(QDateTime(from.startOfDay()).toMSecsSinceEpoch())
#else
                           .arg(QDateTime(from).toMSecsSinceEpoch())
#endif
                           .arg(limitString),
                           {chatId.getByteArray(), chatId.getByteArray()},
                           rowCallback);

    db->execNow(query);

    return dateIdxs;
}

/**
 * @brief Marks a message as delivered.
 * Removing message from the faux-offline pending messages list.
 *
 * @param id Message ID.
 */
void History::markAsDelivered(RowId messageId)
{
    if (historyAccessBlocked()) {
        return;
    }

    db->execLater(QString("DELETE FROM faux_offline_pending WHERE id=%1;").arg(messageId.get()));
}

/**
* @brief Determines if history access should be blocked
* @return True if history should not be accessed
*/
bool History::historyAccessBlocked()
{
    if (!settings.getEnableLogging()) {
        assert(false);
        qCritical() << "Blocked history access while history is disabled";
        return true;
    }

    if (!isValid()) {
        return true;
    }

    return false;
}

void History::markAsBroken(RowId messageId, BrokenMessageReason reason)
{
    if (!isValid()) {
        return;
    }

    QVector<RawDatabase::Query> queries;
    queries += RawDatabase::Query(QString("DELETE FROM faux_offline_pending WHERE id=%1;").arg(messageId.get()));
    queries += RawDatabase::Query(QString("INSERT INTO broken_messages (id, reason) "
                                          "VALUES (%1, %2);")
                                          .arg(messageId.get())
                                          .arg(static_cast<int64_t>(reason)));

    db->execLater(queries);
}
