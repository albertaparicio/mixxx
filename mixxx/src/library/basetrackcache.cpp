// basetrackcache.cpp
// Created 7/3/2011 by RJ Ryan (rryan@mit.edu)

#include "library/basetrackcache.h"

#include "library/trackcollection.h"

namespace {

const bool sDebug = false;

const QHash<QString, int> buildReverseIndex(const QList<QString> items) {
    int i = 0;
    QHash<QString, int> index;
    foreach (const QString item, items) {
        index[item] = i++;
    }
    return index;
}

}  // namespace

BaseTrackCache::BaseTrackCache(TrackCollection* pTrackCollection,
                               QString tableName,
                               QString idColumn,
                               QList<QString> columns,
                               bool isCaching)
        : QObject(),
          m_tableName(tableName),
          m_idColumn(idColumn),
          m_columns(columns),
          m_columnsJoined(m_columns.join(",")),
          m_columnIndex(buildReverseIndex(m_columns)),
          m_bIsCaching(isCaching),
          m_bIndexBuilt(false),
          m_pTrackCollection(pTrackCollection),
          m_trackDAO(m_pTrackCollection->getTrackDAO()),
          m_database(m_pTrackCollection->getDatabase()) {
    m_searchColumns << "artist"
                    << "album"
                    << "location"
                    << "comment"
                    << "title";

    // Convert all the search column names to their field indexes because we use
    // them a bunch.
    m_searchColumnIndices.resize(m_searchColumns.size());
    for (int i = 0; i < m_searchColumns.size(); ++i) {
        m_searchColumnIndices[i] = m_columnIndex.value(m_searchColumns[i], -1);
    }

    buildIndex();
}

BaseTrackCache::~BaseTrackCache() {
}

const QStringList BaseTrackCache::columns() const {
    return m_columns;
}

int BaseTrackCache::columnCount() const {
    return m_columns.size();
}

int BaseTrackCache::fieldIndex(const QString columnName) const {
    return m_columnIndex.value(columnName, -1);
}

void BaseTrackCache::slotTracksAdded(QSet<int> trackIds) {
    if (sDebug) {
        qDebug() << this << "slotTracksAdded" << trackIds.size();
    }
    updateTracksInIndex(trackIds);
}

void BaseTrackCache::slotTracksRemoved(QSet<int> trackIds) {
    if (sDebug) {
        qDebug() << this << "slotTracksRemoved" << trackIds.size();
    }
    foreach (int trackId, trackIds) {
        m_trackInfo.remove(trackId);
    }
}

void BaseTrackCache::slotTrackDirty(int trackId) {
    if (sDebug) {
        qDebug() << this << "slotTrackDirty" << trackId;
    }
    m_dirtyTracks.insert(trackId);
}

void BaseTrackCache::slotTrackChanged(int trackId) {
    if (sDebug) {
        qDebug() << this << "slotTrackChanged" << trackId;
    }
    QSet<int> trackIds;
    trackIds.insert(trackId);
    emit(tracksChanged(trackIds));
}

void BaseTrackCache::slotTrackClean(int trackId) {
    if (sDebug) {
        qDebug() << this << "slotTrackClean" << trackId;
    }
    m_dirtyTracks.remove(trackId);
    updateTrackInIndex(trackId);
}

bool BaseTrackCache::isCached(int trackId) const {
    return m_trackInfo.contains(trackId);
}

void BaseTrackCache::ensureCached(int trackId) {
    updateTrackInIndex(trackId);
}

void BaseTrackCache::ensureCached(QSet<int> trackIds) {
    updateTracksInIndex(trackIds);
}

TrackPointer BaseTrackCache::lookupCachedTrack(int trackId) const {
    // Only get the Track from the TrackDAO if it's in the cache
    if (m_bIsCaching) {
        return m_trackDAO.getTrack(trackId, true);
    }
    return TrackPointer();
}

bool BaseTrackCache::updateIndexWithQuery(QString queryString) {
    QTime timer;
    timer.start();

    if (sDebug) {
        qDebug() << "updateIndexWithQuery issuing query:" << queryString;
    }

    QSqlQuery query(m_database);
    // This causes a memory savings since QSqlCachedResult (what QtSQLite uses)
    // won't allocate a giant in-memory table that we won't use at all.
    query.setForwardOnly(true); // performance improvement?
    query.prepare(queryString);

    if (!query.exec()) {
        qDebug() << this << "updateIndexWithQuery error:"
                 << __FILE__ << __LINE__
                 << query.executedQuery() << query.lastError();
        return false;
    }

    int numColumns = columnCount();
    int idColumn = query.record().indexOf(m_idColumn);

    while (query.next()) {
        int id = query.value(idColumn).toInt();

        QVector<QVariant>& record = m_trackInfo[id];
        record.resize(numColumns);

        for (int i = 0; i < numColumns; ++i) {
            record[i] = query.value(i);
        }
    }

    qDebug() << "Columns:" << numColumns << "Rows:" << m_trackInfo.size();
    qDebug() << this << "updateIndexWithQuery took" << timer.elapsed() << "ms";
    return true;
}

void BaseTrackCache::buildIndex() {
    if (sDebug) {
        qDebug() << this << "buildIndex()";
    }

    QString queryString = QString("SELECT %1 FROM %2")
            .arg(m_columnsJoined).arg(m_tableName);

    if (sDebug) {
        qDebug() << this << "buildIndex query:" << queryString;
    }

    // TODO(rryan) for very large tables, it probably makes more sense to NOT
    // clear the table, and keep track of what IDs we see, then delete the ones
    // we don't see.
    m_trackInfo.clear();

    if (!updateIndexWithQuery(queryString)) {
        qDebug() << "buildIndex failed!";
    }

    m_bIndexBuilt = true;
}

void BaseTrackCache::updateTrackInIndex(int trackId) {
    QSet<int> trackIds;
    trackIds.insert(trackId);
    updateTracksInIndex(trackIds);
}

void BaseTrackCache::updateTracksInIndex(QSet<int> trackIds) {
    if (trackIds.size() == 0) {
        return;
    }

    QStringList idStrings;
    foreach (int trackId, trackIds) {
        idStrings << QVariant(trackId).toString();
    }

    QString queryString = QString("SELECT %1 FROM %2 WHERE %3 in (%4)")
            .arg(m_columnsJoined)
            .arg(m_tableName)
            .arg(m_idColumn)
            .arg(idStrings.join(","));

    if (sDebug) {
        qDebug() << this << "updateTracksInIndex update query:" << queryString;
    }

    if (!updateIndexWithQuery(queryString)) {
        qDebug() << "updateTracksInIndex failed!";
        return;
    }
    emit(tracksChanged(trackIds));
}

QVariant BaseTrackCache::getTrackValueForColumn(TrackPointer pTrack, int column) const {
    if (!pTrack || column < 0) {
        return QVariant();
    }

    // TODO(XXX) Qt properties could really help here.
    // TODO(rryan) this is all TrackDAO specific. What about iTunes/RB/etc.?
    if (fieldIndex(LIBRARYTABLE_ARTIST) == column) {
        return QVariant(pTrack->getArtist());
    } else if (fieldIndex(LIBRARYTABLE_TITLE) == column) {
        return QVariant(pTrack->getTitle());
    } else if (fieldIndex(LIBRARYTABLE_ALBUM) == column) {
        return QVariant(pTrack->getAlbum());
    } else if (fieldIndex(LIBRARYTABLE_YEAR) == column) {
        return QVariant(pTrack->getYear());
    } else if (fieldIndex(LIBRARYTABLE_DATETIMEADDED) == column) {
        return QVariant(pTrack->getDateAdded());
    } else if (fieldIndex(LIBRARYTABLE_GENRE) == column) {
        return QVariant(pTrack->getGenre());
    } else if (fieldIndex(LIBRARYTABLE_FILETYPE) == column) {
        return QVariant(pTrack->getType());
    } else if (fieldIndex(LIBRARYTABLE_TRACKNUMBER) == column) {
        return QVariant(pTrack->getTrackNumber());
    } else if (fieldIndex(LIBRARYTABLE_LOCATION) == column) {
        return QVariant(pTrack->getLocation());
    } else if (fieldIndex(LIBRARYTABLE_COMMENT) == column) {
        return QVariant(pTrack->getComment());
    } else if (fieldIndex(LIBRARYTABLE_DURATION) == column) {
        return pTrack->getDuration();
    } else if (fieldIndex(LIBRARYTABLE_BITRATE) == column) {
        return QVariant(pTrack->getBitrate());
    } else if (fieldIndex(LIBRARYTABLE_BPM) == column) {
        return QVariant(pTrack->getBpm());
    } else if (fieldIndex(LIBRARYTABLE_PLAYED) == column) {
        return QVariant(pTrack->getPlayed());
    } else if (fieldIndex(LIBRARYTABLE_TIMESPLAYED) == column) {
        return QVariant(pTrack->getTimesPlayed());
    } else if (fieldIndex(LIBRARYTABLE_RATING) == column) {
        return pTrack->getRating();
    } else if (fieldIndex(LIBRARYTABLE_KEY) == column) {
        return pTrack->getKey();
    }
    return QVariant();
}

QVariant BaseTrackCache::data(int trackId, int column) const {
    QVariant result;

    // TODO(rryan): allow as an argument
    TrackPointer pTrack;

    // The caller can optionally provide a pTrack if they already looked it
    // up. This is just an optimization to help reduce the # of calls to
    // lookupCachedTrack. If they didn't provide it, look it up.
    if (!pTrack) {
        pTrack = lookupCachedTrack(trackId);
    }
    if (pTrack) {
        result = getTrackValueForColumn(pTrack, column);
    }

    // If the track lookup failed (could happen for track properties we dont
    // keep track of in Track, like playlist position) look up the value in
    // the track info cache.

    // TODO(rryan) this code is flawed for columns that contains row-specific
    // metadata. Currently the upper-levels will not delegate row-specific
    // columns to this method, but there should still be a check here I think.
    if (!result.isValid()) {
        QHash<int, QVector<QVariant> >::const_iterator it =
                m_trackInfo.find(trackId);
        if (it != m_trackInfo.end()) {
            const QVector<QVariant>& fields = it.value();
            result = fields.value(column, result);
        }
    }
    return result;
}

bool BaseTrackCache::trackMatches(const TrackPointer& pTrack,
                                  const QRegExp& matcher) const {
    // For every search column, lookup the value for the track and check
    // if it matches the search query.
    int i = 0;
    foreach (QString column, m_searchColumns) {
        int columnIndex = m_searchColumnIndices[i++];
        QVariant value = getTrackValueForColumn(pTrack, columnIndex);
        if (value.isValid() && qVariantCanConvert<QString>(value)) {
            QString valueStr = value.toString();
            if (valueStr.contains(matcher)) {
                return true;
            }
        }
    }
    return false;
}

void BaseTrackCache::filterAndSort(const QSet<int>& trackIds,
                                   QString searchQuery,
                                   QString extraFilter, int sortColumn,
                                   Qt::SortOrder sortOrder,
                                   QHash<int, int>* trackToIndex) {
    QStringList idStrings;

    if (sortColumn < 0 || sortColumn >= columnCount()) {
        qDebug() << "ERROR: Invalid sort column provided to BaseTrackCache::filterAndSort";
        return;
    }

    // TODO(rryan) consider making this the data passed in and a separate
    // QVector for output
    QSet<int> dirtyTracks;
    foreach (int trackId, trackIds) {
        idStrings << QVariant(trackId).toString();
        if (m_dirtyTracks.contains(trackId)) {
            dirtyTracks.insert(trackId);
        }
    }

    QString filter = filterClause(searchQuery, extraFilter, idStrings);
    QString orderBy = orderByClause(sortColumn, sortOrder);
    QString queryString = QString("SELECT %1 FROM %2 %3 %4")
            .arg(m_idColumn).arg(m_tableName).arg(filter).arg(orderBy);

    if (sDebug) {
        qDebug() << this << "select() executing:" << queryString;
    }

    QSqlQuery query(m_database);
    // This causes a memory savings since QSqlCachedResult (what QtSQLite uses)
    // won't allocate a giant in-memory table that we won't use at all.
    query.setForwardOnly(true);
    query.prepare(queryString);

    if (!query.exec()) {
        qDebug() << this << "select() error:" << __FILE__ << __LINE__
                 << query.executedQuery() << query.lastError();
    }

    QSqlRecord record = query.record();
    int idColumn = record.indexOf(m_idColumn);
    int rows = query.size();

    if (sDebug) {
        qDebug() << "Rows returned:" << rows;
    }

    m_trackOrder.resize(0);
    trackToIndex->clear();
    if (rows > 0) {
        trackToIndex->reserve(rows);
        m_trackOrder.reserve(rows);
    }

    while (query.next()) {
        int id = query.value(idColumn).toInt();
        (*trackToIndex)[id] = m_trackOrder.size();
        m_trackOrder.push_back(id);
    }

    // At this point, the original set of tracks have been divided into two
    // pieces: those that should be in the result set and those that should
    // not. Unfortunately, due to TrackDAO caching, there may be tracks in
    // either category that are there incorrectly. We must look at all the dirty
    // tracks (within the original set, if specified) and evaluate whether they
    // would match or not match the given filter criteria. Once we correct the
    // membership of tracks in either set, we must then insertion-sort the
    // missing tracks into the resulting index list.

    if (dirtyTracks.size() == 0) {
        return;
    }

    // Make a regular expression that matches the query terms.
    QStringList searchTokens = searchQuery.split(" ");
    // Escape every token to stuff in a regular expression
    for (int i = 0; i < searchTokens.size(); ++i) {
        searchTokens[i] = QRegExp::escape(searchTokens[i].trimmed());
    }
    QRegExp searchMatcher(searchTokens.join("|"), Qt::CaseInsensitive);

    foreach (int trackId, dirtyTracks) {
        // Only get the track if it is in the cache.
        TrackPointer pTrack = lookupCachedTrack(trackId);

        if (!pTrack) {
            continue;
        }

        // The track should be in the result set if the search is empty or the
        // track matches the search.
        bool shouldBeInResultSet = searchQuery.isEmpty() ||
                trackMatches(pTrack, searchMatcher);

        // If the track is in this result set.
        bool isInResultSet = trackToIndex->contains(trackId);

        if (shouldBeInResultSet) {
            // Track should be in result set...

            // Remove the track from the results first (we have to do this or it
            // will sort wrong).
            if (isInResultSet) {
                int index = (*trackToIndex)[trackId];
                m_trackOrder.remove(index);
                // Don't update trackToIndex, since we do it below.
            }

            // Figure out where it is supposed to sort. The table is sorted by
            // the sort column, so we can binary search.
            int insertRow = findSortInsertionPoint(pTrack, sortColumn,
                                                   sortOrder, m_trackOrder);

            if (sDebug) {
                qDebug() << this
                         << "Insertion sort says it should be inserted at:"
                         << insertRow;
            }

            // The track should sort at insertRow
            m_trackOrder.insert(insertRow, trackId);

            trackToIndex->clear();
            // Fix the index. TODO(rryan) find a non-stupid way to do this.
            for (int i = 0; i < m_trackOrder.size(); ++i) {
                (*trackToIndex)[m_trackOrder[i]] = i;
            }
        } else if (isInResultSet) {
            // Track should not be in this result set, but it is. We need to
            // remove it.
            int index = (*trackToIndex)[trackId];
            m_trackOrder.remove(index);

            trackToIndex->clear();
            // Fix the index. TODO(rryan) find a non-stupid way to do this.
            for (int i = 0; i < m_trackOrder.size(); ++i) {
                (*trackToIndex)[m_trackOrder[i]] = i;
            }
        }
    }
}


QString BaseTrackCache::filterClause(QString query, QString extraFilter,
                                     QStringList idStrings) const {
    QStringList queryFragments;

    if (!extraFilter.isNull() && extraFilter != "") {
        queryFragments << QString("(%1)").arg(extraFilter);
    }

    if (idStrings.size() > 0) {
        queryFragments << QString("%1 in (%2)")
                .arg(m_idColumn).arg(idStrings.join(","));
    }

    if (!query.isNull() && query != "") {
        QStringList tokens = query.split(" ");
        QSqlField search("search", QVariant::String);

        QStringList tokenFragments;
        foreach (QString token, tokens) {
            token = token.trimmed();
            search.setValue("%" + token + "%");
            QString escapedToken = m_database.driver()->formatValue(search);

            QStringList columnFragments;
            foreach (QString column, m_searchColumns) {
                columnFragments << QString("%1 LIKE %2").arg(column).arg(escapedToken);
            }
            tokenFragments << QString("(%1)").arg(columnFragments.join(" OR "));
        }
        queryFragments << QString("(%1)").arg(tokenFragments.join(" AND "));
    }

    if (queryFragments.size() > 0) {
        return "WHERE " + queryFragments.join(" AND ");
    }
    return "";
}

QString BaseTrackCache::orderByClause(int sortColumn,
                                      Qt::SortOrder sortOrder) const {
    // This is all stolen from QSqlTableModel::orderByClause(), just rigged to
    // sort case-insensitively.

    // TODO(rryan) I couldn't get QSqlRecord to work without exec'ing this damn
    // query. Need to find out how to make it work without exec()'ing and remove
    // this.
    QSqlQuery query(m_database);
    QString queryString = QString("SELECT %1 FROM %2 LIMIT 1")
            .arg(m_columnsJoined).arg(m_tableName);
    query.prepare(queryString);
    query.exec();

    QString s;
    QSqlField f = query.record().field(sortColumn);
    if (!f.isValid()) {
        if (sDebug) {
            qDebug() << "field not valid";
        }
        return QString();
    }

    QString field = m_database.driver()->escapeIdentifier(
        f.name(), QSqlDriver::FieldName);

    s.append(QLatin1String("ORDER BY "));
    QString sort_field = QString("%1.%2").arg(m_tableName).arg(field);

    // If the field is a string, sort using its lowercase form so sort is
    // case-insensitive.
    QVariant::Type type = f.type();

    // TODO(XXX) Instead of special-casing tracknumber here, we should ask the
    // child class to format the expression for sorting.
    if (sort_field.contains("tracknumber")) {
        sort_field = QString("cast(%1 as integer)").arg(sort_field);
    } else if (type == QVariant::String) {
        sort_field = QString("lower(%1)").arg(sort_field);
    }
    s.append(sort_field);

    s += (sortOrder == Qt::AscendingOrder) ? QLatin1String(" ASC") :
            QLatin1String(" DESC");
    return s;
}

int BaseTrackCache::findSortInsertionPoint(TrackPointer pTrack,
                                           const int sortColumn,
                                           Qt::SortOrder sortOrder,
                                           const QVector<int> trackIds) const {
    QVariant trackValue = getTrackValueForColumn(pTrack, sortColumn);

    int min = 0;
    int max = trackIds.size()-1;

    if (sDebug) {
        qDebug() << this << "Trying to insertion sort:"
                 << trackValue << "min" << min << "max" << max;
    }

    while (min <= max) {
        int mid = min + (max - min) / 2;
        int otherTrackId = trackIds[mid];

        // This should not happen, but it's a recoverable error so we should only log it.
        if (!m_trackInfo.contains(otherTrackId)) {
            qDebug() << "WARNING: track" << otherTrackId << "was not in index";
            //updateTrackInIndex(otherTrackId);
        }

        QVariant tableValue = data(otherTrackId, sortColumn);
        int compare = compareColumnValues(sortColumn, sortOrder, trackValue, tableValue);

        if (sDebug) {
            qDebug() << this << "Comparing" << trackValue
                     << "to" << tableValue << ":" << compare;
        }

        if (compare == 0) {
            // Alright, if we're here then we can insert it here and be
            // "correct"
            min = mid;
            break;
        } else if (compare > 0) {
            min = mid + 1;
        } else {
            max = mid - 1;
        }
    }
    return min;
}

int BaseTrackCache::compareColumnValues(int sortColumn, Qt::SortOrder sortOrder,
                                        QVariant val1, QVariant val2) const {
    int result = 0;

    if (sortColumn == fieldIndex(PLAYLISTTRACKSTABLE_POSITION) ||
        sortColumn == fieldIndex(LIBRARYTABLE_BITRATE) ||
        sortColumn == fieldIndex(LIBRARYTABLE_BPM) ||
        sortColumn == fieldIndex(LIBRARYTABLE_DURATION) ||
        sortColumn == fieldIndex(LIBRARYTABLE_TIMESPLAYED) ||
        sortColumn == fieldIndex(LIBRARYTABLE_RATING)) {
        // Sort as floats.
        double delta = val1.toDouble() - val2.toDouble();

        if (fabs(delta) < .00001)
            result = 0;
        else if (delta > 0.0)
            result = 1;
        else
            result = -1;
    } else {
        // Default to case-insensitive string comparison
        result = val1.toString().compare(val2.toString(), Qt::CaseInsensitive);
    }

    // If we're in descending order, flip the comparison.
    if (sortOrder == Qt::DescendingOrder) {
        result = -result;
    }

    return result;
}
