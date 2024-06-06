#include "database.h"

#include "embeddings.h"
#include "modellist.h"
#include "mysettings.h"
#include "network.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileSystemWatcher>
#include <QIODevice>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextStream>
#include <QTimer>
#include <QVariant>
#include <Qt>
#include <QtLogging>

#include <cmath>
#include <utility>
#include <vector>

using namespace Qt::Literals::StringLiterals;

//#define DEBUG
//#define DEBUG_EXAMPLE

static int s_batchSize = 100;

const auto INSERT_CHUNK_SQL = QLatin1String(R"(
    insert into chunks(document_id, chunk_text,
        file, title, author, subject, keywords, page, line_from, line_to, words)
        values(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )");

const auto INSERT_CHUNK_FTS_SQL = QLatin1String(R"(
    insert into chunks_fts(document_id, chunk_id, chunk_text,
        file, title, author, subject, keywords, page, line_from, line_to)
        values(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )");

const auto DELETE_CHUNKS_SQL = QLatin1String(R"(
    delete from chunks WHERE document_id = ?;
    )");

const auto DELETE_CHUNKS_FTS_SQL = QLatin1String(R"(
    delete from chunks_fts WHERE document_id = ?;
    )");

const auto CHUNKS_SQL = QLatin1String(R"(
    create table chunks(document_id integer, chunk_id integer primary key autoincrement, chunk_text varchar,
        file varchar, title varchar, author varchar, subject varchar, keywords varchar,
        page integer, line_from integer, line_to integer, words integer default 0, tokens integer default 0,
        has_embedding integer default 0);
    )");

const auto FTS_CHUNKS_SQL = QLatin1String(R"(
    create virtual table chunks_fts using fts5(document_id unindexed, chunk_id unindexed, chunk_text,
        file, title, author, subject, keywords, page, line_from, line_to, tokenize="trigram");
    )");

const auto SELECT_CHUNKS_BY_DOCUMENT_SQL = QLatin1String(R"(
    select chunk_id from chunks WHERE document_id = ?;
    )");

const auto SELECT_CHUNKS_SQL = QLatin1String(R"(
    select chunks.chunk_id, documents.document_time,
        chunks.chunk_text, chunks.file, chunks.title, chunks.author, chunks.page,
        chunks.line_from, chunks.line_to
    from chunks
    join documents ON chunks.document_id = documents.id
    join folders ON documents.folder_id = folders.id
    join collections ON folders.id = collections.folder_id
    where chunks.chunk_id in (%1) and collections.collection_name in (%2);
)");

const auto SELECT_NGRAM_SQL = QLatin1String(R"(
    select chunks_fts.chunk_id, documents.document_time,
        chunks_fts.chunk_text, chunks_fts.file, chunks_fts.title, chunks_fts.author, chunks_fts.page,
        chunks_fts.line_from, chunks_fts.line_to
    from chunks_fts
    join documents ON chunks_fts.document_id = documents.id
    join folders ON documents.folder_id = folders.id
    join collections ON folders.id = collections.folder_id
    where chunks_fts match ? and collections.collection_name in (%1)
    order by bm25(chunks_fts)
    limit %2;
    )");

const auto SELECT_FILE_FOR_CHUNK_SQL = QLatin1String(R"(
    select c.file
    from chunks c
    where c.chunk_id = ?;
    )");

bool selectFileForChunk(QSqlQuery &q, int chunk_id, QString &file) {
    if (!q.prepare(SELECT_FILE_FOR_CHUNK_SQL))
        return false;
    q.addBindValue(chunk_id);
    if (!q.exec())
        return false;
    if (!q.next())
        return false;
    file = q.value(0).toString();
    return true;
}

const auto SELECT_UNCOMPLETED_CHUNKS_SQL = QLatin1String(R"(
    select c.chunk_id, c.chunk_text as chunk, d.folder_id
    from chunks c
    join documents d ON c.document_id = d.id
    where c.has_embedding != 1 and d.folder_id = ?;
    )");

const auto SELECT_COUNT_CHUNKS_SQL = QLatin1String(R"(
    select count(c.chunk_id) as total_chunks
    from chunks c
    join documents d on c.document_id = d.id
    where d.folder_id = ?;
    )");

const auto UPDATE_CHUNK_HAS_EMBEDDING = QLatin1String(R"(
    update chunks set has_embedding = 1 where chunk_id = ?;
    )");

bool addChunk(QSqlQuery &q, int document_id, const QString &chunk_text,
    const QString &file, const QString &title, const QString &author, const QString &subject, const QString &keywords,
    int page, int from, int to, int words, int *chunk_id)
{
    {
        if (!q.prepare(INSERT_CHUNK_SQL))
            return false;
        q.addBindValue(document_id);
        q.addBindValue(chunk_text);
        q.addBindValue(file);
        q.addBindValue(title);
        q.addBindValue(author);
        q.addBindValue(subject);
        q.addBindValue(keywords);
        q.addBindValue(page);
        q.addBindValue(from);
        q.addBindValue(to);
        q.addBindValue(words);
        if (!q.exec())
            return false;
    }
    if (!q.exec("select last_insert_rowid();"))
        return false;
    if (!q.next())
        return false;
    *chunk_id = q.value(0).toInt();
    {
        if (!q.prepare(INSERT_CHUNK_FTS_SQL))
            return false;
        q.addBindValue(document_id);
        q.addBindValue(*chunk_id);
        q.addBindValue(chunk_text);
        q.addBindValue(file);
        q.addBindValue(title);
        q.addBindValue(author);
        q.addBindValue(subject);
        q.addBindValue(keywords);
        q.addBindValue(page);
        q.addBindValue(from);
        q.addBindValue(to);
        if (!q.exec())
            return false;
    }
    return true;
}

bool removeChunksByDocumentId(QSqlQuery &q, int document_id)
{
    {
        if (!q.prepare(DELETE_CHUNKS_SQL))
            return false;
        q.addBindValue(document_id);
        if (!q.exec())
            return false;
    }

    {
        if (!q.prepare(DELETE_CHUNKS_FTS_SQL))
            return false;
        q.addBindValue(document_id);
        if (!q.exec())
            return false;
    }

    return true;
}

bool selectAllUncompletedChunks(QSqlQuery &q, int folder_id, QList<EmbeddingChunk> &chunks) {
    if (!q.prepare(SELECT_UNCOMPLETED_CHUNKS_SQL))
        return false;
    q.addBindValue(folder_id);
    if (!q.exec())
        return false;
    while (q.next()) {
        EmbeddingChunk i;
        i.chunk_id = q.value(0).toInt();
        i.chunk = q.value(1).toString();
        i.folder_id = q.value(2).toInt();
        chunks.append(i);
    }
    return true;
}

bool selectCountChunks(QSqlQuery &q, int folder_id, int &count) {
    if (!q.prepare(SELECT_COUNT_CHUNKS_SQL))
        return false;
    q.addBindValue(folder_id);
    if (!q.exec())
        return false;
    if (!q.next()) {
        count = 0;
        return false;
    }
    count = q.value(0).toInt();
    return true;
}

bool updateChunkHasEmbedding(QSqlQuery &q, int chunk_id) {
    if (!q.prepare(UPDATE_CHUNK_HAS_EMBEDDING))
        return false;
    q.addBindValue(chunk_id);
    if (!q.exec())
        return false;
    return true;
}

QStringList generateGrams(const QString &input, int N)
{
    // Remove common English punctuation using QRegularExpression
    static QRegularExpression punctuation(R"([.,;:!?'"()\-])");
    QString cleanedInput = input;
    cleanedInput = cleanedInput.remove(punctuation);

    // Split the cleaned input into words using whitespace
    static QRegularExpression spaces("\\s+");
    QStringList words = cleanedInput.split(spaces, Qt::SkipEmptyParts);
    N = qMin(words.size(), N);

    // Generate all possible N-grams
    QStringList ngrams;
    for (int i = 0; i < words.size() - (N - 1); ++i) {
        QStringList currentNgram;
        for (int j = 0; j < N; ++j) {
            currentNgram.append("\"" + words[i + j] + "\"");
        }
        ngrams.append("NEAR(" + currentNgram.join(" ") + ", " + QString::number(N) + ")");
    }
    return ngrams;
}

bool selectChunk(QSqlQuery &q, const QList<QString> &collection_names, const std::vector<qint64> &chunk_ids, int retrievalSize)
{
    QString chunk_ids_str = QString::number(chunk_ids[0]);
    for (size_t i = 1; i < chunk_ids.size(); ++i)
        chunk_ids_str += "," + QString::number(chunk_ids[i]);
    const QString collection_names_str = collection_names.join("', '");
    const QString formatted_query = SELECT_CHUNKS_SQL.arg(chunk_ids_str).arg("'" + collection_names_str + "'");
    if (!q.prepare(formatted_query))
        return false;
    return q.exec();
}

bool selectChunk(QSqlQuery &q, const QList<QString> &collection_names, const QString &chunk_text, int retrievalSize)
{
    static QRegularExpression spaces("\\s+");
    const int N_WORDS = chunk_text.split(spaces).size();
    for (int N = N_WORDS; N > 2; N--) {
        // first try trigrams
        QList<QString> text = generateGrams(chunk_text, N);
        QString orText = text.join(" OR ");
        const QString collection_names_str = collection_names.join("', '");
        const QString formatted_query = SELECT_NGRAM_SQL.arg("'" + collection_names_str + "'").arg(QString::number(retrievalSize));
        if (!q.prepare(formatted_query))
            return false;
        q.addBindValue(orText);
        bool success = q.exec();
        if (!success) return false;
        if (q.next()) {
#if defined(DEBUG)
            qDebug() << "hit on" << N << "before" << chunk_text << "after" << orText;
#endif
            q.previous();
            return true;
        }
    }
    return true;
}

const auto INSERT_COLLECTION_SQL = QLatin1String(R"(
    insert into collections(collection_name, folder_id, last_update_time, embedding_model, force_indexing) values(?, ?, ?, ?, ?);
    )");

const auto DELETE_COLLECTION_SQL = QLatin1String(R"(
    delete from collections where collection_name = ? and folder_id = ?;
    )");

const auto COLLECTIONS_SQL = QLatin1String(R"(
    create table collections(collection_name varchar, folder_id integer, last_update_time integer, embedding_model varchar, force_indexing integer, unique(collection_name, folder_id));
    )");

const auto SELECT_FOLDERS_FROM_COLLECTIONS_SQL = QLatin1String(R"(
    select f.id, f.folder_path
    from collections c
    join folders f on c.folder_id = f.id
    where collection_name = ?;
    )");

const auto SELECT_COLLECTIONS_FROM_FOLDER_SQL = QLatin1String(R"(
    select collection_name from collections where folder_id = ?;
    )");

const auto SELECT_COLLECTIONS_SQL_V1 = QLatin1String(R"(
    select c.collection_name, f.folder_path, f.id
    from collections c
    join folders f on c.folder_id = f.id
    order by c.collection_name asc, f.folder_path asc;
    )");

const auto SELECT_COLLECTIONS_SQL_V2 = QLatin1String(R"(
    select c.collection_name, f.folder_path, f.id, c.last_update_time, c.embedding_model, c.force_indexing
    from collections c
    join folders f on c.folder_id = f.id
    order by c.collection_name asc, f.folder_path asc;
    )");

const auto UPDATE_COLLECTION_FORCE_INDEXING = QLatin1String(R"(
    update collections
    set force_indexing = 0
    where collection_name = ?;
    )");

bool addCollection(QSqlQuery &q, const QString &collection_name, int folder_id,
                   const QDateTime &last_update,
                   const QString &embedding_model,
                   bool force_indexing)
{
    if (!q.prepare(INSERT_COLLECTION_SQL))
        return false;
    q.addBindValue(collection_name);
    q.addBindValue(folder_id);
    q.addBindValue(last_update);
    q.addBindValue(embedding_model);
    q.addBindValue(force_indexing);
    return q.exec();
}

bool removeCollection(QSqlQuery &q, const QString &collection_name, int folder_id)
{
    if (!q.prepare(DELETE_COLLECTION_SQL))
        return false;
    q.addBindValue(collection_name);
    q.addBindValue(folder_id);
    return q.exec();
}

bool selectFoldersFromCollection(QSqlQuery &q, const QString &collection_name, QList<QPair<int, QString>> *folders) {
    if (!q.prepare(SELECT_FOLDERS_FROM_COLLECTIONS_SQL))
        return false;
    q.addBindValue(collection_name);
    if (!q.exec())
        return false;
    while (q.next())
        folders->append({q.value(0).toInt(), q.value(1).toString()});
    return true;
}

bool selectCollectionsFromFolder(QSqlQuery &q, int folder_id, QList<QString> *collections) {
    if (!q.prepare(SELECT_COLLECTIONS_FROM_FOLDER_SQL))
        return false;
    q.addBindValue(folder_id);
    if (!q.exec())
        return false;
    while (q.next())
        collections->append(q.value(0).toString());
    return true;
}

static bool selectAllFromCollections(QSqlQuery &q, QList<CollectionItem> *collections, int version = LOCALDOCS_VERSION) {

    switch (version) {
    case 1:
        if (!q.prepare(SELECT_COLLECTIONS_SQL_V1))
            return false;
        break;
    case 2:
        if (!q.prepare(SELECT_COLLECTIONS_SQL_V2))
            return false;
        break;
    default:
        Q_UNREACHABLE();
        return false;
    }

    if (!q.exec())
        return false;
    while (q.next()) {
        CollectionItem i;
        i.collection = q.value(0).toString();
        i.folder_path = q.value(1).toString();
        i.folder_id = q.value(2).toInt();
        i.indexing = false;
        i.installed = true;

        if (version > 1) {
            i.lastUpdate = q.value(3).toDateTime();
            i.embeddingModel = q.value(4).toString();
            i.forceIndexing = q.value(5).toBool();
        }

        // We force indexing flag if the version does not match
        if (version < LOCALDOCS_VERSION)
            i.forceIndexing = true;

        collections->append(i);
    }
    return true;
}

bool updateCollectionForceIndexing(QSqlQuery &q, const QString &collection_name) {
    if (!q.prepare(UPDATE_COLLECTION_FORCE_INDEXING))
        return false;
    q.addBindValue(collection_name);
    return q.exec();
}

const auto INSERT_FOLDERS_SQL = QLatin1String(R"(
    insert into folders(folder_path) values(?);
    )");

const auto DELETE_FOLDERS_SQL = QLatin1String(R"(
    delete from folders where id = ?;
    )");

const auto SELECT_FOLDERS_FROM_PATH_SQL = QLatin1String(R"(
    select id from folders where folder_path = ?;
    )");

const auto SELECT_FOLDERS_FROM_ID_SQL = QLatin1String(R"(
    select folder_path from folders where id = ?;
    )");

const auto SELECT_ALL_FOLDERPATHS_SQL = QLatin1String(R"(
    select folder_path from folders;
    )");

const auto FOLDERS_SQL = QLatin1String(R"(
    create table folders(id integer primary key, folder_path varchar unique);
    )");

bool addFolderToDB(QSqlQuery &q, const QString &folder_path, int *folder_id)
{
    if (!q.prepare(INSERT_FOLDERS_SQL))
        return false;
    q.addBindValue(folder_path);
    if (!q.exec())
        return false;
    *folder_id = q.lastInsertId().toInt();
    return true;
}

bool removeFolderFromDB(QSqlQuery &q, int folder_id) {
    if (!q.prepare(DELETE_FOLDERS_SQL))
        return false;
    q.addBindValue(folder_id);
    return q.exec();
}

bool selectFolder(QSqlQuery &q, const QString &folder_path, int *id) {
    if (!q.prepare(SELECT_FOLDERS_FROM_PATH_SQL))
        return false;
    q.addBindValue(folder_path);
    if (!q.exec())
        return false;
    Q_ASSERT(q.size() < 2);
    if (q.next())
        *id = q.value(0).toInt();
    return true;
}

bool selectFolder(QSqlQuery &q, int id, QString *folder_path) {
    if (!q.prepare(SELECT_FOLDERS_FROM_ID_SQL))
        return false;
    q.addBindValue(id);
    if (!q.exec())
        return false;
    Q_ASSERT(q.size() < 2);
    if (q.next())
        *folder_path = q.value(0).toString();
    return true;
}

bool selectAllFolderPaths(QSqlQuery &q, QList<QString> *folder_paths) {
    if (!q.prepare(SELECT_ALL_FOLDERPATHS_SQL))
        return false;
    if (!q.exec())
        return false;
    while (q.next())
        folder_paths->append(q.value(0).toString());
    return true;
}

const auto INSERT_DOCUMENTS_SQL = QLatin1String(R"(
    insert into documents(folder_id, document_time, document_path) values(?, ?, ?);
    )");

const auto UPDATE_DOCUMENT_TIME_SQL = QLatin1String(R"(
    update documents set document_time = ? where id = ?;
    )");

const auto DELETE_DOCUMENTS_SQL = QLatin1String(R"(
    delete from documents where id = ?;
    )");

const auto DOCUMENTS_SQL = QLatin1String(R"(
    create table documents(id integer primary key, folder_id integer, document_time integer, document_path varchar unique);
    )");

const auto SELECT_DOCUMENT_SQL = QLatin1String(R"(
    select id, document_time from documents where document_path = ?;
    )");

const auto SELECT_DOCUMENTS_SQL = QLatin1String(R"(
    select id from documents where folder_id = ?;
    )");

const auto SELECT_ALL_DOCUMENTS_SQL = QLatin1String(R"(
    select id, document_path from documents;
    )");

const auto SELECT_COUNT_STATISTICS_SQL = QLatin1String(R"(
    select count(distinct d.id) as total_docs, sum(c.words) as total_words, sum(c.tokens) as total_tokens
    from documents d
    left join chunks c on d.id = c.document_id
    where d.folder_id = ?;
    )");

bool addDocument(QSqlQuery &q, int folder_id, qint64 document_time, const QString &document_path, int *document_id)
{
    if (!q.prepare(INSERT_DOCUMENTS_SQL))
        return false;
    q.addBindValue(folder_id);
    q.addBindValue(document_time);
    q.addBindValue(document_path);
    if (!q.exec())
        return false;
    *document_id = q.lastInsertId().toInt();
    return true;
}

bool removeDocument(QSqlQuery &q, int document_id) {
    if (!q.prepare(DELETE_DOCUMENTS_SQL))
        return false;
    q.addBindValue(document_id);
    return q.exec();
}

bool updateDocument(QSqlQuery &q, int id, qint64 document_time)
{
    if (!q.prepare(UPDATE_DOCUMENT_TIME_SQL))
        return false;
    q.addBindValue(document_time);
    q.addBindValue(id);
    return q.exec();
}

bool selectDocument(QSqlQuery &q, const QString &document_path, int *id, qint64 *document_time) {
    if (!q.prepare(SELECT_DOCUMENT_SQL))
        return false;
    q.addBindValue(document_path);
    if (!q.exec())
        return false;
    Q_ASSERT(q.size() < 2);
    if (q.next()) {
        *id = q.value(0).toInt();
        *document_time = q.value(1).toLongLong();
    }
    return true;
}

bool selectDocuments(QSqlQuery &q, int folder_id, QList<int> *documentIds) {
    if (!q.prepare(SELECT_DOCUMENTS_SQL))
        return false;
    q.addBindValue(folder_id);
    if (!q.exec())
        return false;
    while (q.next())
        documentIds->append(q.value(0).toInt());
    return true;
}

bool selectCountStatistics(QSqlQuery &q, int folder_id, int *total_docs, int *total_words, int *total_tokens) {
    if (!q.prepare(SELECT_COUNT_STATISTICS_SQL))
        return false;
    q.addBindValue(folder_id);
    if (!q.exec())
        return false;
    if (q.next()) {
        *total_docs = q.value(0).toInt();
        *total_words = q.value(1).toInt();
        *total_tokens = q.value(2).toInt();
    }
    return true;
}

void Database::transaction()
{
    bool ok = m_db.transaction();
    Q_ASSERT(ok);
}

void Database::commit()
{
    bool ok = m_db.commit();
    Q_ASSERT(ok);
}

void Database::rollback()
{
    bool ok = m_db.rollback();
    Q_ASSERT(ok);
}

bool Database::hasContent()
{
    return m_db.tables().contains("chunks", Qt::CaseInsensitive);
}

int Database::openDatabase(const QString &modelPath, bool create, int ver)
{
    if (m_db.isOpen())
        m_db.close();
    auto dbPath = u"%1/localdocs_v%2.db"_s.arg(modelPath).arg(ver);
    if (!create && !QFileInfo(dbPath).exists())
        return 0;
    m_db.setDatabaseName(dbPath);
    if (!m_db.open()) {
        qWarning() << "ERROR: opening db" << m_db.lastError().text();
        return -1;
    }
    return hasContent();
}

bool Database::openLatestDb(const QString &modelPath, QList<CollectionItem> &oldCollections)
{
    /*
     * Support upgrade path from older versions:
     *
     *  1. Detect and load dbPath with older versions
     *  2. Provide versioned SQL select statements
     *  3. Upgrade the tables to the new version
     *  4. By default mark all collections of older versions as force indexing and present to the user
     *     the an 'update' button letting them know a breaking change happened and that the collection
     *     will need to be indexed again
     */

    int dbVer;
    for (dbVer = LOCALDOCS_VERSION;; dbVer--) {
        if (dbVer < LOCALDOCS_MIN_VER) return true; // create a new db
        int res = openDatabase(modelPath, false, dbVer);
        if (res == 1) break; // found one with content
        if (res == -1) return false; // error
    }

    if (dbVer == LOCALDOCS_VERSION) return true; // already up-to-date

    // If we're upgrading, then we need to do a select on the current version of the collections table,
    // then create the new one and populate the collections table and mark them as needing forced
    // indexing

#if defined(DEBUG)
    qDebug() << "Older localdocs version found" << dbVer << "upgrade to" << LOCALDOCS_VERSION;
#endif

    // Select the current collections which will be marked to force indexing
    QSqlQuery q(m_db);
    if (!selectAllFromCollections(q, &oldCollections, dbVer)) {
        qWarning() << "ERROR: Could not open select old collections" << q.lastError();
        return false;
    }

    m_db.close();
    return true;
}

bool Database::initDb(const QString &modelPath, const QList<CollectionItem> &oldCollections)
{
    if (!m_db.isOpen()) {
        int res = openDatabase(modelPath);
        if (res == 1) return true; // already populated
        if (res == -1) return false; // error
    } else if (hasContent()) {
        return true; // already populated
    }

    transaction();

    QSqlQuery q(m_db);
    if (!q.exec(CHUNKS_SQL)) {
        qWarning() << "ERROR: failed to create chunks table" << q.lastError().text();
        rollback();
        return false;
    }

    if (!q.exec(FTS_CHUNKS_SQL)) {
        qWarning() << "ERROR: failed to create fts chunks table" << q.lastError().text();
        rollback();
        return false;
    }

    if (!q.exec(COLLECTIONS_SQL)) {
        qWarning() << "ERROR: failed to create collections table" << q.lastError().text();
        rollback();
        return false;
    }

    if (!q.exec(FOLDERS_SQL)) {
        qWarning() << "ERROR: failed to create folders table" << q.lastError().text();
        rollback();
        return false;
    }

    if (!q.exec(DOCUMENTS_SQL)) {
        qWarning() << "ERROR: failed to create documents table" << q.lastError().text();
        rollback();
        return false;
    }

    bool success = true;
    for (const CollectionItem &item : oldCollections)
        success &= addForcedCollection(item);

    if (!success) {
        qWarning() << "ERROR: failed to add previous collections to new database";
        rollback();
        return false;
    }

    commit();
    return true;
}

Database::Database(int chunkSize)
    : QObject(nullptr)
    , m_chunkSize(chunkSize)
    , m_scanTimer(new QTimer(this))
    , m_watcher(new QFileSystemWatcher(this))
    , m_embLLM(new EmbeddingLLM)
    , m_embeddings(new Embeddings(this))
    , m_databaseValid(true)
{
    m_db = QSqlDatabase::database(QSqlDatabase::defaultConnection, false);
    if (!m_db.isValid())
        m_db = QSqlDatabase::addDatabase("QSQLITE");
    Q_ASSERT(m_db.isValid());

    moveToThread(&m_dbThread);
    m_dbThread.setObjectName("database");
    m_dbThread.start();
}

Database::~Database()
{
    m_dbThread.quit();
    m_dbThread.wait();
    delete m_embLLM;
}

CollectionItem Database::guiCollectionItem(int folder_id) const
{
    Q_ASSERT(m_collectionMap.contains(folder_id));
    return m_collectionMap.value(folder_id);
}

void Database::updateGuiForCollectionItem(const CollectionItem &item)
{
    m_collectionMap.insert(item.folder_id, item);
    emit requestUpdateGuiForCollectionItem(item);
}

void Database::addGuiCollectionItem(const CollectionItem &item)
{
    m_collectionMap.insert(item.folder_id, item);
    emit requestAddGuiCollectionItem(item);
}

void Database::removeGuiFolderById(int folder_id)
{
    m_collectionMap.remove(folder_id);
    emit requestRemoveGuiFolderById(folder_id);
}

void Database::guiCollectionListUpdated(const QList<CollectionItem> &collectionList)
{
    for (const auto &i : collectionList)
        m_collectionMap.insert(i.folder_id, i);
    emit requestGuiCollectionListUpdated(collectionList);
}

void Database::scheduleNext(int folder_id, size_t countForFolder)
{
    CollectionItem item = guiCollectionItem(folder_id);
    item.currentDocsToIndex = countForFolder;
    if (!countForFolder) {
        sendChunkList(); // send any remaining embedding chunks to llm
        item.indexing = false;
        item.installed = true;
    }
    updateGuiForCollectionItem(item);
}

void Database::handleDocumentError(const QString &errorMessage,
    int document_id, const QString &document_path, const QSqlError &error)
{
    qWarning() << errorMessage << document_id << document_path << error.text();
}

size_t Database::chunkStream(QTextStream &stream, int folder_id, int document_id, const QString &file,
    const QString &title, const QString &author, const QString &subject, const QString &keywords, int page,
    int maxChunks)
{
    int charCount = 0;
    int line_from = -1;
    int line_to = -1;
    QList<QString> words;
    int chunks = 0;
    int addedWords = 0;

    CollectionItem item = guiCollectionItem(folder_id);
    item.fileCurrentlyProcessing = file;

    while (!stream.atEnd()) {
        QString word;
        stream >> word;
        charCount += word.length();
        if (!word.isEmpty())
            words.append(word);
        if (charCount + words.size() - 1 >= m_chunkSize || stream.atEnd()) {
            const QString chunk = words.join(" ");
            QSqlQuery q(m_db);
            int chunk_id = 0;
            if (!addChunk(q,
                document_id,
                chunk,
                file,
                title,
                author,
                subject,
                keywords,
                page,
                line_from,
                line_to,
                words.size(),
                &chunk_id
            )) {
                qWarning() << "ERROR: Could not insert chunk into db" << q.lastError();
            }

            addedWords += words.size();

            EmbeddingChunk toEmbed;
            toEmbed.folder_id = folder_id;
            toEmbed.chunk_id = chunk_id;
            toEmbed.chunk = chunk;
            appendChunk(toEmbed);
            ++chunks;

            words.clear();
            charCount = 0;

            if (maxChunks > 0 && chunks == maxChunks)
                break;
        }
    }

    if (chunks) {
        item = guiCollectionItem(folder_id);
        item.totalEmbeddingsToIndex += chunks;
        item.totalWords += addedWords;
        updateGuiForCollectionItem(item);
    }

    return stream.pos();
}

void Database::appendChunk(const EmbeddingChunk &chunk)
{
    m_chunkList.reserve(s_batchSize);
    m_chunkList.append(chunk);
    if (m_chunkList.size() >= s_batchSize)
        sendChunkList();
}

void Database::sendChunkList() {
    m_embLLM->generateAsyncEmbeddings(m_chunkList);
    m_chunkList.clear();
}

void Database::handleEmbeddingsGenerated(const QVector<EmbeddingResult> &embeddings)
{
    if (embeddings.isEmpty())
        return;

    // FIXME: Replace this with an arrow file on disk
    // FIXME: Add the tokens information
    int folder_id = 0;
    QSqlQuery q(m_db);
    for (auto e : embeddings) {
        folder_id = e.folder_id;
        if (!m_embeddings->add(e.embedding, e.chunk_id))
            qWarning() << "ERROR: Cannot add point to embeddings index";
        else {
            updateChunkHasEmbedding(q, e.chunk_id);
        }
    }

    QString file;
    if (!selectFileForChunk(q, embeddings.first().chunk_id, file))
        qWarning() << "ERROR: Cannot find file for chunk";

    CollectionItem item = guiCollectionItem(folder_id);
    item.currentEmbeddingsToIndex += embeddings.count();
    item.fileCurrentlyProcessing = file;
    updateGuiForCollectionItem(item);
    m_embeddings->save();
}

void Database::handleErrorGenerated(int folder_id, const QString &error)
{
    CollectionItem item = guiCollectionItem(folder_id);
    item.error = error;
    updateGuiForCollectionItem(item);
}

bool Database::getChunksByDocumentId(int document_id, QList<int> &chunkIds)
{
    QSqlQuery q(m_db);

    if (!q.prepare(SELECT_CHUNKS_BY_DOCUMENT_SQL)) {
        qWarning() << "ERROR: Cannot prepare sql for select chunks by document" << q.lastError();
        return false;
    }

    q.addBindValue(document_id);

    if (!q.exec()) {
        qWarning() << "ERROR: Cannot exec sql for select chunks by document" << q.lastError();
        return false;
    }

    while (q.next())
        chunkIds.append(q.value(0).toInt());
    return true;
}

size_t Database::countOfDocuments(int folder_id) const
{
    if (!m_docsToScan.contains(folder_id))
        return 0;
    return m_docsToScan.value(folder_id).size();
}

size_t Database::countOfBytes(int folder_id) const
{
    if (!m_docsToScan.contains(folder_id))
        return 0;
    size_t totalBytes = 0;
    const QQueue<DocumentInfo> &docs = m_docsToScan.value(folder_id);
    for (const DocumentInfo &f : docs)
        totalBytes += f.doc.size();
    return totalBytes;
}

DocumentInfo Database::dequeueDocument()
{
    Q_ASSERT(!m_docsToScan.isEmpty());
    const int firstKey = m_docsToScan.firstKey();
    QQueue<DocumentInfo> &queue = m_docsToScan[firstKey];
    Q_ASSERT(!queue.isEmpty());
    DocumentInfo result = queue.dequeue();
    if (queue.isEmpty())
        m_docsToScan.remove(firstKey);
    return result;
}

void Database::removeFolderFromDocumentQueue(int folder_id)
{
    if (!m_docsToScan.contains(folder_id))
        return;
    m_docsToScan.remove(folder_id);
    removeGuiFolderById(folder_id);
}

void Database::enqueueDocumentInternal(const DocumentInfo &info, bool prepend)
{
    const int key = info.folder;
    if (!m_docsToScan.contains(key))
        m_docsToScan[key] = QQueue<DocumentInfo>();
    if (prepend)
        m_docsToScan[key].prepend(info);
    else
        m_docsToScan[key].enqueue(info);
}

void Database::enqueueDocuments(int folder_id, const QVector<DocumentInfo> &infos)
{
    for (int i = 0; i < infos.size(); ++i)
        enqueueDocumentInternal(infos[i]);
    const size_t count = countOfDocuments(folder_id);

    CollectionItem item = guiCollectionItem(folder_id);
    item.currentDocsToIndex = count;
    item.totalDocsToIndex = count;
    const size_t bytes = countOfBytes(folder_id);
    item.currentBytesToIndex = bytes;
    item.totalBytesToIndex = bytes;
    updateGuiForCollectionItem(item);
    m_scanTimer->start();
}

void Database::scanQueueBatch() {
    QElapsedTimer timer;
    timer.start();

    int scanned = 0;
    int lastToScan = m_docsToScan.count();

    transaction();

    // scan for up to 100ms or until we run out of documents
    QList<int> chunksToRemove;
    while (!m_docsToScan.isEmpty() && timer.elapsed() < 100) {
        ++scanned;
        if (!scanQueue(chunksToRemove)) {
            rollback();
            return;
        }
    }

    // failure is no longer an option, apply everything at once and hope this is effectively atomic
    for (const auto &chunk: chunksToRemove)
        m_embeddings->remove(chunk);
    commit();
    if (!chunksToRemove.isEmpty())
        m_embeddings->save();
}

bool Database::scanQueue(QList<int> &chunksToRemove)
{
    DocumentInfo info = dequeueDocument();
    const size_t countForFolder = countOfDocuments(info.folder);
    const int folder_id = info.folder;

    // Update info
    info.doc.stat();

    // If the doc has since been deleted or no longer readable, then we schedule more work and return
    // leaving the cleanup for the cleanup handler
    if (!info.doc.exists() || !info.doc.isReadable()) {
        scheduleNext(folder_id, countForFolder);
        return true;
    }

    const qint64 document_time = info.doc.fileTime(QFile::FileModificationTime).toMSecsSinceEpoch();
    const QString document_path = info.doc.canonicalFilePath();
    const bool currentlyProcessing = info.currentlyProcessing;

    // Check and see if we already have this document
    QSqlQuery q(m_db);
    int existing_id = -1;
    qint64 existing_time = -1;
    if (!selectDocument(q, document_path, &existing_id, &existing_time)) {
        handleDocumentError("ERROR: Cannot select document",
            existing_id, document_path, q.lastError());
        scheduleNext(folder_id, countForFolder);
        return false;
    }

    // If we have the document, we need to compare the last modification time and if it is newer
    // we must rescan the document, otherwise return
    if (existing_id != -1 && !currentlyProcessing) {
        Q_ASSERT(existing_time != -1);
        if (document_time == existing_time) {
            // No need to rescan, but we do have to schedule next
            scheduleNext(folder_id, countForFolder);
            return true;
        }
        if (!getChunksByDocumentId(existing_id, chunksToRemove)) {
            scheduleNext(folder_id, countForFolder);
            return false;
        }
        if (!removeChunksByDocumentId(q, existing_id)) {
            handleDocumentError("ERROR: Cannot remove chunks of document",
                existing_id, document_path, q.lastError());
            scheduleNext(folder_id, countForFolder);
            return false;
        }
        updateCollectionStatistics();
    }

    // Update the document_time for an existing document, or add it for the first time now
    int document_id = existing_id;
    if (!currentlyProcessing) {
        if (document_id != -1) {
            if (!updateDocument(q, document_id, document_time)) {
                handleDocumentError("ERROR: Could not update document_time",
                    document_id, document_path, q.lastError());
                scheduleNext(folder_id, countForFolder);
                return false;
            }
        } else {
            if (!addDocument(q, folder_id, document_time, document_path, &document_id)) {
                handleDocumentError("ERROR: Could not add document",
                    document_id, document_path, q.lastError());
                scheduleNext(folder_id, countForFolder);
                return false;
            }

            CollectionItem item = guiCollectionItem(folder_id);
            item.totalDocs += 1;
            updateGuiForCollectionItem(item);
        }
    }

    Q_ASSERT(document_id != -1);
    if (info.isPdf()) {
        QPdfDocument doc;
        if (QPdfDocument::Error::None != doc.load(info.doc.canonicalFilePath())) {
            handleDocumentError("ERROR: Could not load pdf",
                document_id, document_path, q.lastError());
            scheduleNext(folder_id, countForFolder);
            return false;
        }
        const size_t bytes = info.doc.size();
        const size_t bytesPerPage = std::floor(bytes / doc.pageCount());
        const int pageIndex = info.currentPage;
#if defined(DEBUG)
        qDebug() << "scanning page" << pageIndex << "of" << doc.pageCount() << document_path;
#endif
        const QPdfSelection selection = doc.getAllText(pageIndex);
        QString text = selection.text();
        QTextStream stream(&text);
        chunkStream(stream, info.folder, document_id, info.doc.fileName(),
            doc.metaData(QPdfDocument::MetaDataField::Title).toString(),
            doc.metaData(QPdfDocument::MetaDataField::Author).toString(),
            doc.metaData(QPdfDocument::MetaDataField::Subject).toString(),
            doc.metaData(QPdfDocument::MetaDataField::Keywords).toString(),
            pageIndex + 1
        );
        CollectionItem item = guiCollectionItem(info.folder);
        item.currentBytesToIndex -= bytesPerPage;
        updateGuiForCollectionItem(item);
        if (info.currentPage < doc.pageCount()) {
            info.currentPage += 1;
            info.currentlyProcessing = true;
            enqueueDocumentInternal(info, true /*prepend*/);
            scheduleNext(folder_id, countForFolder + 1);
            return true;
        }

        item.currentBytesToIndex -= bytes - (bytesPerPage * doc.pageCount());
        updateGuiForCollectionItem(item);
    } else {
        QFile file(document_path);
        if (!file.open(QIODevice::ReadOnly)) {
            handleDocumentError("ERROR: Cannot open file for scanning",
                                existing_id, document_path, q.lastError());
            scheduleNext(folder_id, countForFolder);
            return false;
        }

        const size_t bytes = info.doc.size();
        QTextStream stream(&file);
        const size_t byteIndex = info.currentPosition;
        if (!stream.seek(byteIndex)) {
            handleDocumentError("ERROR: Cannot seek to pos for scanning",
                                existing_id, document_path, q.lastError());
            scheduleNext(folder_id, countForFolder);
            return false;
        }
#if defined(DEBUG)
        qDebug() << "scanning byteIndex" << byteIndex << "of" << bytes << document_path;
#endif
        int pos = chunkStream(stream, info.folder, document_id, info.doc.fileName(), QString() /*title*/, QString() /*author*/,
            QString() /*subject*/, QString() /*keywords*/, -1 /*page*/, 100 /*maxChunks*/);
        file.close();
        const size_t bytesChunked = pos - byteIndex;
        CollectionItem item = guiCollectionItem(info.folder);
        item.currentBytesToIndex -= bytesChunked;
        updateGuiForCollectionItem(item);
        if (info.currentPosition < bytes) {
            info.currentPosition = pos;
            info.currentlyProcessing = true;
            enqueueDocumentInternal(info, true /*prepend*/);
            scheduleNext(folder_id, countForFolder + 1);
            return true;
        }
    }

    scheduleNext(folder_id, countForFolder);
    return true;
}

void Database::scanDocuments(int folder_id, const QString &folder_path)
{
#if defined(DEBUG)
    qDebug() << "scanning folder for documents" << folder_path;
#endif

    // FIXME_BLOCKER: This should be configurable
    static const QList<QString> extensions { "txt", "pdf", "md", "rst" };

    QDir dir(folder_path);
    Q_ASSERT(dir.exists());
    Q_ASSERT(dir.isReadable());
    QDirIterator it(folder_path, QDir::Readable | QDir::Files, QDirIterator::Subdirectories);
    QVector<DocumentInfo> infos;
    while (it.hasNext()) {
        it.next();
        QFileInfo fileInfo = it.fileInfo();
        if (fileInfo.isDir()) {
            addFolderToWatch(fileInfo.canonicalFilePath());
            continue;
        }

        if (!extensions.contains(fileInfo.suffix()))
            continue;

        DocumentInfo info;
        info.folder = folder_id;
        info.doc = fileInfo;
        infos.append(info);
    }

    if (!infos.isEmpty()) {
        CollectionItem item = guiCollectionItem(folder_id);
        item.indexing = true;
        updateGuiForCollectionItem(item);
        enqueueDocuments(folder_id, infos);
    }
}

void Database::start()
{
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, &Database::directoryChanged);
    connect(m_embLLM, &EmbeddingLLM::embeddingsGenerated, this, &Database::handleEmbeddingsGenerated);
    connect(m_embLLM, &EmbeddingLLM::errorGenerated, this, &Database::handleErrorGenerated);
    m_scanTimer->callOnTimeout(this, &Database::scanQueueBatch);

    const QString modelPath = MySettings::globalInstance()->modelPath();
    QList<CollectionItem> oldCollections;

    if (!openLatestDb(modelPath, oldCollections)) {
        m_databaseValid = false;
    } else if (!initDb(modelPath, oldCollections)) {
        m_databaseValid = false;
    } else if (m_embeddings->fileExists() && !m_embeddings->load()) {
        qWarning() << "ERROR: Could not load embeddings";
        m_databaseValid = false;
    } else {
        addCurrentFolders();
    }

    if (!m_databaseValid)
        emit databaseValidChanged();
}

void Database::addCurrentFolders()
{
#if defined(DEBUG)
    qDebug() << "addCurrentFolders";
#endif

    QSqlQuery q(m_db);
    QList<CollectionItem> collections;
    if (!selectAllFromCollections(q, &collections)) {
        qWarning() << "ERROR: Cannot select collections" << q.lastError();
        return;
    }

    guiCollectionListUpdated(collections);

    for (const auto &i : collections) {
        if (!i.forceIndexing) {
            scheduleUncompletedEmbeddings(i.folder_id);
            addFolder(i.collection, i.folder_path);
        }
    }

    updateCollectionStatistics();
}

void Database::scheduleUncompletedEmbeddings(int folder_id)
{
    QList<EmbeddingChunk> chunkList;
    QSqlQuery q(m_db);
    if (!selectAllUncompletedChunks(q, folder_id, chunkList)) {
        qWarning() << "ERROR: Cannot select uncompleted chunks" << q.lastError();
        return;
    }

    if (chunkList.isEmpty())
        return;

    int total = 0;
    if (!selectCountChunks(q, folder_id, total)) {
        qWarning() << "ERROR: Cannot count total chunks" << q.lastError();
        return;
    }

    CollectionItem item = guiCollectionItem(folder_id);
    item.totalEmbeddingsToIndex = total;
    item.currentEmbeddingsToIndex = total - chunkList.size();
    updateGuiForCollectionItem(item);

    for (int i = 0; i < chunkList.size(); i += s_batchSize) {
        QList<EmbeddingChunk> batch = chunkList.mid(i, s_batchSize);
        m_embLLM->generateAsyncEmbeddings(batch);
    }
}

void Database::updateCollectionStatistics()
{
    QSqlQuery q(m_db);
    QList<CollectionItem> collections;
    if (!selectAllFromCollections(q, &collections)) {
        qWarning() << "ERROR: Cannot select collections" << q.lastError();
        return;
    }

    for (const auto &i : collections) {
        int total_docs = 0;
        int total_words = 0;
        int total_tokens = 0;
        if (!selectCountStatistics(q, i.folder_id, &total_docs, &total_words, &total_tokens)) {
            qWarning() << "ERROR: could not count statistics for folder" << q.lastError();
        } else {
            CollectionItem item = guiCollectionItem(i.folder_id);
            item.totalDocs = total_docs;
            item.totalWords = total_words;
            item.totalTokens = total_tokens;
            updateGuiForCollectionItem(item);
        }
    }
}

int Database::checkAndAddFolderToDB(const QString &path)
{
    QFileInfo info(path);
    if (!info.exists() || !info.isReadable()) {
        qWarning() << "ERROR: Cannot add folder that doesn't exist or not readable" << path;
        return -1;
    }

    QSqlQuery q(m_db);
    int folder_id = -1;

    // See if the folder exists in the db
    if (!selectFolder(q, path, &folder_id)) {
        qWarning() << "ERROR: Cannot select folder from path" << path << q.lastError();
        return -1;
    }

    // Add the folder
    if (folder_id == -1 && !addFolderToDB(q, path, &folder_id)) {
        qWarning() << "ERROR: Cannot add folder to db with path" << path << q.lastError();
        return -1;
    }

    Q_ASSERT(folder_id != -1);
    return folder_id;
}

bool Database::addForcedCollection(const CollectionItem &collection)
{
    // These are collection items that came from an older version of localdocs which require
    // forced indexing that should only be done when the user has explicitly asked for them to be
    // indexed again
    const QString path = collection.folder_path;

    const int folder_id = checkAndAddFolderToDB(path);
    if (folder_id == -1)
        return false;

    const QString model = m_embLLM->model();
    if (model.isEmpty()) {
        qWarning() << "ERROR: We have no embedding model";
        return false;
    }

    QSqlQuery q(m_db);
    if (!addCollection(q, collection.collection, folder_id,
                       QDateTime() /*last_update*/,
                       model /*embedding_model*/,
                       true /*force_indexing*/)) {
        qWarning() << "ERROR: Cannot add folder to collection" << collection.collection << path << q.lastError();
        return false;
    }

    addGuiCollectionItem(collection);
    return true;
}

void Database::forceIndexing(const QString &collection)
{
    QSqlQuery q(m_db);
    QList<QPair<int, QString>> folders;
    if (!selectFoldersFromCollection(q, collection, &folders)) {
        qWarning() << "ERROR: Cannot select folders from collections" << collection << q.lastError();
        return;
    }

    if (!updateCollectionForceIndexing(q, collection)) {
        qWarning() << "ERROR: Cannot update collection" << collection << q.lastError();
        return;
    }

    for (const auto& folder : folders) {
        CollectionItem item = guiCollectionItem(folder.first);
        item.forceIndexing = false;
        updateGuiForCollectionItem(item);
        addFolder(collection, folder.second);
    }
}

bool containsFolderId(const QList<QPair<int, QString>> &folders, int folder_id) {
    for (const auto& folder : folders)
        if (folder.first == folder_id)
            return true;
    return false;
}

void Database::addFolder(const QString &collection, const QString &path)
{
    const int folder_id = checkAndAddFolderToDB(path);
    if (folder_id == -1)
        return;

    // See if the folder has already been added to the collection
    QSqlQuery q(m_db);
    QList<QPair<int, QString>> folders;
    if (!selectFoldersFromCollection(q, collection, &folders)) {
        qWarning() << "ERROR: Cannot select folders from collections" << collection << q.lastError();
        return;
    }

    const QString model = m_embLLM->model();
    if (model.isEmpty()) {
        qWarning() << "ERROR: We have no embedding model";
        return;
    }

    if (!containsFolderId(folders, folder_id)) {
        if (!addCollection(q, collection, folder_id,
                           QDateTime() /*last_update*/,
                           model /*embedding_model*/,
                           false /*force_indexing*/)) {
            qWarning() << "ERROR: Cannot add folder to collection" << collection << path << q.lastError();
            return;
        }

        CollectionItem i;
        i.collection = collection;
        i.folder_path = path;
        i.folder_id = folder_id;
        addGuiCollectionItem(i);
    }

    addFolderToWatch(path);
    scanDocuments(folder_id, path);
}

void Database::removeFolder(const QString &collection, const QString &path)
{
#if defined(DEBUG)
    qDebug() << "removeFolder" << path;
#endif

    QSqlQuery q(m_db);
    int folder_id = -1;

    // See if the folder exists in the db
    if (!selectFolder(q, path, &folder_id)) {
        qWarning() << "ERROR: Cannot select folder from path" << path << q.lastError();
        return;
    }

    // If we don't have a folder_id in the db, then something bad has happened
    Q_ASSERT(folder_id != -1);
    if (folder_id == -1) {
        qWarning() << "ERROR: Collected folder does not exist in db" << path;
        m_watcher->removePath(path);
        return;
    }

    removeFolderInternal(collection, folder_id, path);
}

void Database::removeFolderInternal(const QString &collection, int folder_id, const QString &path)
{
    // Determine if the folder is used by more than one collection
    QSqlQuery q(m_db);
    QList<QString> collections;
    if (!selectCollectionsFromFolder(q, folder_id, &collections)) {
        qWarning() << "ERROR: Cannot select collections from folder" << folder_id << q.lastError();
        return;
    }

    transaction();

    // Remove it from the collections
    if (!removeCollection(q, collection, folder_id)) {
        qWarning() << "ERROR: Cannot remove collection" << collection << folder_id << q.lastError();
        return rollback();
    }

    // If the folder is associated with more than one collection, then return
    if (collections.count() > 1)
        return commit();

    // First remove all upcoming jobs associated with this folder
    removeFolderFromDocumentQueue(folder_id);

    // Get a list of all documents associated with folder
    QList<int> documentIds;
    if (!selectDocuments(q, folder_id, &documentIds)) {
        qWarning() << "ERROR: Cannot select documents" << folder_id << q.lastError();
        return rollback();
    }

    // Remove all chunks and documents associated with this folder
    QList<int> chunksToRemove;
    for (int document_id : documentIds) {
        if (!getChunksByDocumentId(document_id, chunksToRemove))
            return rollback();
        if (!removeChunksByDocumentId(q, document_id)) {
            qWarning() << "ERROR: Cannot remove chunks of document_id" << document_id << q.lastError();
            return rollback();
        }

        if (!removeDocument(q, document_id)) {
            qWarning() << "ERROR: Cannot remove document_id" << document_id << q.lastError();
            return rollback();
        }
    }

    if (!removeFolderFromDB(q, folder_id)) {
        qWarning() << "ERROR: Cannot remove folder_id" << folder_id << q.lastError();
        return rollback();
    }

    // failure is no longer an option, apply everything at once and hope this is effectively atomic
    // TODO(jared): check the embeddings file for stale entries on startup
    for (const auto &chunk: chunksToRemove)
        m_embeddings->remove(chunk);
    commit();
    if (!chunksToRemove.isEmpty())
        m_embeddings->save();

    removeGuiFolderById(folder_id);
    removeFolderFromWatch(path);
}

bool Database::addFolderToWatch(const QString &path)
{
#if defined(DEBUG)
    qDebug() << "addFolderToWatch" << path;
#endif
    return m_watcher->addPath(path);
}

bool Database::removeFolderFromWatch(const QString &path)
{
#if defined(DEBUG)
    qDebug() << "removeFolderFromWatch" << path;
#endif
    return m_watcher->removePath(path);
}

void Database::retrieveFromDB(const QList<QString> &collections, const QString &text, int retrievalSize,
    QList<ResultInfo> *results)
{
#if defined(DEBUG)
    qDebug() << "retrieveFromDB" << collections << text << retrievalSize;
#endif

    QSqlQuery q(m_db);
    if (m_embeddings->isLoaded()) {
        std::vector<float> result = m_embLLM->generateEmbeddings(text);
        if (result.empty()) {
            qDebug() << "ERROR: generating embeddings returned a null result";
            return;
        }
        std::vector<qint64> embeddings = m_embeddings->search(result, retrievalSize);
        if (!selectChunk(q, collections, embeddings, retrievalSize)) {
            qDebug() << "ERROR: selecting chunks:" << q.lastError().text();
            return;
        }
    } else {
        if (!selectChunk(q, collections, text, retrievalSize)) {
            qDebug() << "ERROR: selecting chunks:" << q.lastError().text();
            return;
        }
    }

    while (q.next()) {
#if defined(DEBUG)
        const int rowid = q.value(0).toInt();
#endif
        const QString chunk_text = q.value(2).toString();
        const QString date = QDateTime::fromMSecsSinceEpoch(q.value(1).toLongLong()).toString("yyyy, MMMM dd");
        const QString file = q.value(3).toString();
        const QString title = q.value(4).toString();
        const QString author = q.value(5).toString();
        const int page = q.value(6).toInt();
        const int from =q.value(7).toInt();
        const int to =q.value(8).toInt();
        ResultInfo info;
        info.file = file;
        info.title = title;
        info.author = author;
        info.date = date;
        info.text = chunk_text;
        info.page = page;
        info.from = from;
        info.to = to;
        results->append(info);
#if defined(DEBUG)
        qDebug() << "retrieve rowid:" << rowid
                 << "chunk_text:" << chunk_text;
#endif
    }
}

void Database::cleanDB()
{
#if defined(DEBUG)
    qDebug() << "cleanDB";
#endif

    // Scan all folders in db to make sure they still exist
    QSqlQuery q(m_db);
    QList<CollectionItem> collections;
    if (!selectAllFromCollections(q, &collections)) {
        qWarning() << "ERROR: Cannot select collections" << q.lastError();
        return;
    }

    for (const auto &i : collections) {
        // Find the path for the folder
        QFileInfo info(i.folder_path);
        if (!info.exists() || !info.isReadable()) {
#if defined(DEBUG)
            qDebug() << "clean db removing folder" << i.folder_id << i.folder_path;
#endif
            removeFolderInternal(i.collection, i.folder_id, i.folder_path);
        }
    }

    // Scan all documents in db to make sure they still exist
    if (!q.prepare(SELECT_ALL_DOCUMENTS_SQL)) {
        qWarning() << "ERROR: Cannot prepare sql for select all documents" << q.lastError();
        return;
    }

    if (!q.exec()) {
        qWarning() << "ERROR: Cannot exec sql for select all documents" << q.lastError();
        return;
    }

    transaction();

    QList<int> chunksToRemove;
    while (q.next()) {
        int document_id = q.value(0).toInt();
        QString document_path = q.value(1).toString();
        QFileInfo info(document_path);
        if (info.exists() && info.isReadable())
            continue;

#if defined(DEBUG)
        qDebug() << "clean db removing document" << document_id << document_path;
#endif

        // Remove all chunks and documents that either don't exist or have become unreadable
        if (!getChunksByDocumentId(document_id, chunksToRemove))
            return rollback();
        QSqlQuery query(m_db);
        if (!removeChunksByDocumentId(query, document_id)) {
            qWarning() << "ERROR: Cannot remove chunks of document_id" << document_id << query.lastError();
            rollback();
            return updateCollectionStatistics();
        }

        if (!removeDocument(query, document_id)) {
            qWarning() << "ERROR: Cannot remove document_id" << document_id << query.lastError();
            rollback();
            return updateCollectionStatistics();
        }
    }

    // failure is no longer an option, apply everything at once and hope this is effectively atomic
    for (const auto &chunk: chunksToRemove)
        m_embeddings->remove(chunk);
    commit();
    if (!chunksToRemove.isEmpty())
        m_embeddings->save();

    updateCollectionStatistics();
}

void Database::changeChunkSize(int chunkSize)
{
    if (chunkSize == m_chunkSize)
        return;

#if defined(DEBUG)
    qDebug() << "changeChunkSize" << chunkSize;
#endif

    m_chunkSize = chunkSize;

    QSqlQuery q(m_db);
    // Scan all documents in db to make sure they still exist
    if (!q.prepare(SELECT_ALL_DOCUMENTS_SQL)) {
        qWarning() << "ERROR: Cannot prepare sql for select all documents" << q.lastError();
        return;
    }

    if (!q.exec()) {
        qWarning() << "ERROR: Cannot exec sql for select all documents" << q.lastError();
        return;
    }

    transaction();

    QList<int> chunksToRemove;
    while (q.next()) {
        int document_id = q.value(0).toInt();
        // Remove all chunks and documents to change the chunk size
        QSqlQuery query(m_db);
        if (!getChunksByDocumentId(document_id, chunksToRemove))
            return rollback();
        if (!removeChunksByDocumentId(query, document_id)) {
            qWarning() << "ERROR: Cannot remove chunks of document_id" << document_id << query.lastError();
            return rollback();
        }

        if (!removeDocument(query, document_id)) {
            qWarning() << "ERROR: Cannot remove document_id" << document_id << query.lastError();
            return rollback();
        }
    }

    // failure is no longer an option, apply everything at once and hope this is effectively atomic
    for (const auto &chunk: chunksToRemove)
        m_embeddings->remove(chunk);
    commit();
    if (!chunksToRemove.isEmpty())
        m_embeddings->save();

    addCurrentFolders();
    updateCollectionStatistics();
}

void Database::directoryChanged(const QString &path)
{
#if defined(DEBUG)
    qDebug() << "directoryChanged" << path;
#endif

    QSqlQuery q(m_db);
    int folder_id = -1;

    // Lookup the folder_id in the db
    if (!selectFolder(q, path, &folder_id)) {
        qWarning() << "ERROR: Cannot select folder from path" << path << q.lastError();
        return;
    }

    // If we don't have a folder_id in the db, then something bad has happened
    Q_ASSERT(folder_id != -1);
    if (folder_id == -1) {
        qWarning() << "ERROR: Watched folder does not exist in db" << path;
        m_watcher->removePath(path);
        return;
    }

    // Clean the database
    cleanDB();

    // Rescan the documents associated with the folder
    scanDocuments(folder_id, path);
}
