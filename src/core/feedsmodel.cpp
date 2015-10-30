// This file is part of RSS Guard.
//
// Copyright (C) 2011-2015 by Martin Rotter <rotter.martinos@gmail.com>
//
// RSS Guard is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// RSS Guard is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with RSS Guard. If not, see <http://www.gnu.org/licenses/>.

#include "core/feedsmodel.h"

#include "definitions/definitions.h"
#include "services/abstract/feed.h"
#include "services/standard/standardfeed.h"
#include "services/standard/standardcategory.h"
#include "services/standard/standardfeedsimportexportmodel.h"
#include "core/recyclebin.h"
#include "miscellaneous/textfactory.h"
#include "miscellaneous/databasefactory.h"
#include "miscellaneous/iconfactory.h"
#include "miscellaneous/mutex.h"
#include "gui/messagebox.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QPair>
#include <QStack>
#include <QMimeData>
#include <QTimer>

#include <algorithm>


FeedsModel::FeedsModel(QObject *parent)
  : QAbstractItemModel(parent), m_recycleBin(new RecycleBin()), m_autoUpdateTimer(new QTimer(this)) {
  setObjectName(QSL("FeedsModel"));

  // Create root item.
  m_rootItem = new RootItem();
  m_rootItem->setId(NO_PARENT_CATEGORY);

  //: Name of root item of feed list which can be seen in feed add/edit dialog.
  m_rootItem->setTitle(tr("Root"));
  m_rootItem->setIcon(qApp->icons()->fromTheme(QSL("folder-root")));

  // Setup icons.
  m_countsIcon = qApp->icons()->fromTheme(QSL("mail-mark-unread"));

  //: Title text in the feed list header.
  m_headerData << tr("Title");

  m_tooltipData << /*: Feed list header "titles" column tooltip.*/ tr("Titles of feeds/categories.") <<
                   /*: Feed list header "counts" column tooltip.*/ tr("Counts of unread/all meesages.");

  connect(m_autoUpdateTimer, SIGNAL(timeout()), this, SLOT(executeNextAutoUpdate()));

  loadFromDatabase();

  // Setup the timer.
  updateAutoUpdateStatus();
}

FeedsModel::~FeedsModel() {
  qDebug("Destroying FeedsModel instance.");

  // Delete all model items.
  delete m_rootItem;
}

void FeedsModel::quit() {
  if (m_autoUpdateTimer->isActive()) {
    m_autoUpdateTimer->stop();
  }
}

void FeedsModel::executeNextAutoUpdate() {
  if (!qApp->feedUpdateLock()->tryLock()) {
    qDebug("Delaying scheduled feed auto-updates for one minute due to another running update.");

    // Cannot update, quit.
    return;
  }

  // If global auto-update is enabled and its interval counter reached zero,
  // then we need to restore it.
  if (m_globalAutoUpdateEnabled && --m_globalAutoUpdateRemainingInterval < 0) {
    // We should start next auto-update interval.
    m_globalAutoUpdateRemainingInterval = m_globalAutoUpdateInitialInterval;
  }

  qDebug("Starting auto-update event, pass %d/%d.", m_globalAutoUpdateRemainingInterval, m_globalAutoUpdateInitialInterval);

  // Pass needed interval data and lets the model decide which feeds
  // should be updated in this pass.
  QList<Feed*> feeds_for_update = feedsForScheduledUpdate(m_globalAutoUpdateEnabled && m_globalAutoUpdateRemainingInterval == 0);

  qApp->feedUpdateLock()->unlock();

  if (!feeds_for_update.isEmpty()) {
    // Request update for given feeds.
    emit feedsUpdateRequested(feeds_for_update);

    // NOTE: OSD/bubble informing about performing
    // of scheduled update can be shown now.
    qApp->showGuiMessage(tr("Starting auto-update of some feeds"),
                         tr("I will auto-update %n feed(s).", 0, feeds_for_update.size()),
                         QSystemTrayIcon::Information);
  }
}

void FeedsModel::updateAutoUpdateStatus() {
  // Restore global intervals.
  // NOTE: Specific per-feed interval are left intact.
  m_globalAutoUpdateInitialInterval = qApp->settings()->value(GROUP(Feeds), SETTING(Feeds::AutoUpdateInterval)).toInt();
  m_globalAutoUpdateRemainingInterval = m_globalAutoUpdateInitialInterval;
  m_globalAutoUpdateEnabled = qApp->settings()->value(GROUP(Feeds), SETTING(Feeds::AutoUpdateEnabled)).toBool();

  // Start global auto-update timer if it is not running yet.
  // NOTE: The timer must run even if global auto-update
  // is not enabled because user can still enable auto-update
  // for individual feeds.
  if (!m_autoUpdateTimer->isActive()) {
    m_autoUpdateTimer->setInterval(AUTO_UPDATE_INTERVAL);
    m_autoUpdateTimer->start();
    qDebug("Auto-update timer started with interval %d.", m_autoUpdateTimer->interval());
  }
  else {
    qDebug("Auto-update timer is already running.");
  }
}

QVariant FeedsModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation != Qt::Horizontal) {
    return QVariant();
  }

  switch (role) {
    case Qt::DisplayRole:
      if (section == FDS_MODEL_TITLE_INDEX) {
        return m_headerData.at(FDS_MODEL_TITLE_INDEX);
      }
      else {
        return QVariant();
      }

    case Qt::ToolTipRole:
      return m_tooltipData.at(section);

    case Qt::DecorationRole:
      if (section == FDS_MODEL_COUNTS_INDEX) {
        return m_countsIcon;
      }
      else {
        return QVariant();
      }

    default:
      return QVariant();
  }
}

QModelIndex FeedsModel::index(int row, int column, const QModelIndex &parent) const {
  if (!hasIndex(row, column, parent)) {
    return QModelIndex();
  }

  RootItem *parent_item = itemForIndex(parent);
  RootItem *child_item = parent_item->child(row);

  if (child_item) {
    return createIndex(row, column, child_item);
  }
  else {
    return QModelIndex();
  }
}

QModelIndex FeedsModel::parent(const QModelIndex &child) const {
  if (!child.isValid()) {
    return QModelIndex();
  }

  RootItem *child_item = itemForIndex(child);
  RootItem *parent_item = child_item->parent();

  if (parent_item == m_rootItem) {
    return QModelIndex();
  }
  else {
    return createIndex(parent_item->row(), 0, parent_item);
  }
}

int FeedsModel::rowCount(const QModelIndex &parent) const {
  if (parent.column() > 0) {
    return 0;
  }
  else {
    return itemForIndex(parent)->childCount();
  }
}

bool FeedsModel::removeItem(const QModelIndex &index) {
  if (index.isValid()) {
    QModelIndex parent_index = index.parent();
    RootItem *deleting_item = itemForIndex(index);
    RootItem *parent_item = deleting_item->parent();

    // Try to persistently remove the item.
    if (deleting_item->removeItself()) {
      // Item was persistently removed.
      // Remove it from the model.
      beginRemoveRows(parent_index, index.row(), index.row());
      parent_item->removeChild(deleting_item);
      endRemoveRows();

      delete deleting_item;
      return true;
    }
  }

  // Item was not removed successfully.
  return false;
}

bool FeedsModel::addCategory(StandardCategory *category, RootItem *parent) {
  // Get index of parent item (parent standard category).
  QModelIndex parent_index = indexForItem(parent);
  bool result = category->addItself(parent);

  if (result) {
    // Category was added to the persistent storage,
    // so add it to the model.
    beginInsertRows(parent_index, parent->childCount(), parent->childCount());
    parent->appendChild(category);
    endInsertRows();
  }
  else {
    // We cannot delete (*this) in its method, thus delete it here.
    delete category;
  }

  return result;
}

bool FeedsModel::addFeed(StandardFeed *feed, RootItem *parent) {
  // Get index of parent item (parent standard category or root item).
  QModelIndex parent_index = indexForItem(parent);
  bool result = feed->addItself(parent);

  if (result) {
    // Feed was added to the persistent storage so add it to the model.
    beginInsertRows(parent_index, parent->childCount(), parent->childCount());
    parent->appendChild(feed);
    endInsertRows();
  }
  else {
    delete feed;
  }

  return result;
}

void FeedsModel::reassignNodeToNewParent(RootItem *original_node, RootItem *new_parent) {
  RootItem *original_parent = original_node->parent();

  if (original_parent != new_parent) {
    // User edited category and set it new parent item,
    // se we need to move the item in the model too.
    int original_index_of_feed = original_parent->childItems().indexOf(original_node);
    int new_index_of_feed = new_parent->childCount();

    // Remove the original item from the model...
    beginRemoveRows(indexForItem(original_parent), original_index_of_feed, original_index_of_feed);
    original_parent->removeChild(original_node);
    endRemoveRows();

    // ... and insert it under the new parent.
    beginInsertRows(indexForItem(new_parent), new_index_of_feed, new_index_of_feed);
    new_parent->appendChild(original_node);
    endInsertRows();
  }
}

QList<Feed*> FeedsModel::feedsForScheduledUpdate(bool auto_update_now) {
  QList<Feed*> feeds_for_update;

  foreach (Feed *feed, allFeeds()) {
    switch (feed->autoUpdateType()) {
      case StandardFeed::DontAutoUpdate:
        // Do not auto-update this feed ever.
        continue;

      case StandardFeed::DefaultAutoUpdate:
        if (auto_update_now) {
          feeds_for_update.append(feed);
        }

        break;

      case StandardFeed::SpecificAutoUpdate:
      default:
        int remaining_interval = feed->autoUpdateRemainingInterval();

        if (--remaining_interval <= 0) {
          // Interval of this feed passed, include this feed in the output list
          // and reset the interval.
          feeds_for_update.append(feed);
          feed->setAutoUpdateRemainingInterval(feed->autoUpdateInitialInterval());
        }
        else {
          // Interval did not pass, set new decremented interval and do NOT
          // include this feed in the output list.
          feed->setAutoUpdateRemainingInterval(remaining_interval);
        }

        break;
    }
  }

  return feeds_for_update;
}

QList<Message> FeedsModel::messagesForFeeds(const QList<Feed*> &feeds) {
  QList<Message> messages;

  QSqlDatabase database = qApp->database()->connection(objectName(),
                                                       DatabaseFactory::FromSettings);
  QSqlQuery query_read_msg(database);
  query_read_msg.setForwardOnly(true);
  query_read_msg.prepare("SELECT title, url, author, date_created, contents "
                         "FROM Messages "
                         "WHERE is_deleted = 0 AND feed = :feed;");

  foreach (Feed *feed, feeds) {
    query_read_msg.bindValue(QSL(":feed"), feed->id());

    if (query_read_msg.exec()) {
      while (query_read_msg.next()) {
        Message message;

        message.m_feedId = feed->id();
        message.m_title = query_read_msg.value(0).toString();
        message.m_url = query_read_msg.value(1).toString();
        message.m_author = query_read_msg.value(2).toString();
        message.m_created = TextFactory::parseDateTime(query_read_msg.value(3).value<qint64>());
        message.m_contents = query_read_msg.value(4).toString();

        messages.append(message);
      }
    }
  }

  return messages;
}

int FeedsModel::columnCount(const QModelIndex &parent) const {
  Q_UNUSED(parent)

  return FEEDS_VIEW_COLUMN_COUNT;
}

RootItem *FeedsModel::itemForIndex(const QModelIndex &index) const {
  if (index.isValid() && index.model() == this) {
    return static_cast<RootItem*>(index.internalPointer());
  }
  else {
    return m_rootItem;
  }
}

StandardCategory *FeedsModel::categoryForIndex(const QModelIndex &index) const {
  RootItem *item = itemForIndex(index);

  if (item->kind() == RootItem::Cattegory) {
    return item->toCategory();
  }
  else {
    return NULL;
  }
}

RecycleBin *FeedsModel::recycleBinForIndex(const QModelIndex &index) const {
  RootItem *item = itemForIndex(index);

  if (item->kind() == RootItem::Bin) {
    return item->toRecycleBin();
  }
  else {
    return NULL;
  }
}

QModelIndex FeedsModel::indexForItem(RootItem *item) const {
  if (item == NULL || item->kind() == RootItem::Root) {
    // Root item lies on invalid index.
    return QModelIndex();
  }

  QStack<RootItem*> chain;

  while (item->kind() != RootItem::Root) {
    chain.push(item);
    item = item->parent();
  }

  // Now, we have complete chain list: parent --- ..... --- parent --- leaf (item).
  QModelIndex target_index = indexForItem(m_rootItem);

  // We go through the stack and create our target index.
  while (!chain.isEmpty()) {
    RootItem *parent_item = chain.pop();
    target_index = index(parent_item->parent()->childItems().indexOf(parent_item), 0, target_index);
  }

  return target_index;
}

bool FeedsModel::hasAnyFeedNewMessages() {
  foreach (const Feed *feed, allFeeds()) {
    if (feed->status() == StandardFeed::NewMessages) {
      return true;
    }
  }

  return false;
}

bool FeedsModel::mergeModel(FeedsImportExportModel *model, QString &output_message) {
  if (model == NULL || model->rootItem() == NULL) {
    output_message = tr("Invalid tree data.");
    qDebug("Root item for merging two models is null.");
    return false;
  }

  QStack<RootItem*> original_parents; original_parents.push(m_rootItem);
  QStack<RootItem*> new_parents; new_parents.push(model->rootItem());
  bool some_feed_category_error = false;

  // We are definitely about to add some new items into the model.
  //emit layoutAboutToBeChanged();

  // Iterate all new items we would like to merge into current model.
  while (!new_parents.isEmpty()) {
    RootItem *target_parent = original_parents.pop();
    RootItem *source_parent = new_parents.pop();

    foreach (RootItem *source_item, source_parent->childItems()) {
      if (!model->isItemChecked(source_item)) {
        // We can skip this item, because it is not checked and should not be imported.
        // NOTE: All descendants are thus skipped too.
        continue;
      }

      if (source_item->kind() == RootItem::Cattegory) {
        StandardCategory *source_category = source_item->toCategory();
        StandardCategory *new_category = new StandardCategory(*source_category);

        // Add category to model.
        new_category->clearChildren();

        if (addCategory(new_category, target_parent)) {
          // Process all children of this category.
          original_parents.push(new_category);
          new_parents.push(source_category);
        }
        else {
          // Add category failed, but this can mean that the same category (with same title)
          // already exists. If such a category exists in current parent, then find it and
          // add descendants to it.
          RootItem *existing_category = target_parent->child(RootItem::Cattegory, new_category->title());

          if (existing_category != NULL) {
            original_parents.push(existing_category);
            new_parents.push(source_category);
          }
          else {
            some_feed_category_error = true;
          }
        }
      }
      else if (source_item->kind() == RootItem::Feeed) {
        StandardFeed *source_feed = source_item->toFeed();
        StandardFeed *new_feed = new StandardFeed(*source_feed);

        // Append this feed and end this iteration.
        if (!addFeed(new_feed, target_parent)) {
          some_feed_category_error = true;
        }
      }
    }
  }

  // Changes are done now. Finalize the new model.
  //emit layoutChanged();

  if (some_feed_category_error) {
    output_message = tr("Import successfull, but some feeds/categories were not imported due to error.");
  }
  else {
    output_message = tr("Import was completely successfull.");
  }

  return !some_feed_category_error;
}

void FeedsModel::reloadChangedLayout(QModelIndexList list) {
  while (!list.isEmpty()) {
    QModelIndex indx = list.takeFirst();
    QModelIndex indx_parent = indx.parent();

    // Underlying data are changed.
    emit dataChanged(index(indx.row(), 0, indx_parent), index(indx.row(), FDS_MODEL_COUNTS_INDEX, indx_parent));
  }
}

QStringList FeedsModel::textualFeedIds(const QList<Feed*> &feeds) {
  QStringList stringy_ids;
  stringy_ids.reserve(feeds.size());

  foreach (Feed *feed, feeds) {
    stringy_ids.append(QString::number(feed->id()));
  }

  return stringy_ids;
}

void FeedsModel::reloadWholeLayout() {
  emit layoutAboutToBeChanged();
  emit layoutChanged();
}

void FeedsModel::loadFromDatabase() {
  // Delete all childs of the root node and clear them from the memory.
  qDeleteAll(m_rootItem->childItems());
  m_rootItem->clearChildren();

  QSqlDatabase database = qApp->database()->connection(objectName(), DatabaseFactory::FromSettings);
  CategoryAssignment categories;
  FeedAssignment feeds;

  // Obtain data for categories from the database.
  QSqlQuery query_categories(database);
  query_categories.setForwardOnly(true);

  if (!query_categories.exec(QSL("SELECT * FROM Categories;")) || query_categories.lastError().isValid()) {
    qFatal("Query for obtaining categories failed. Error message: '%s'.",
           qPrintable(query_categories.lastError().text()));
  }

  while (query_categories.next()) {
    CategoryAssignmentItem pair;
    pair.first = query_categories.value(CAT_DB_PARENT_ID_INDEX).toInt();
    pair.second = new StandardCategory(query_categories.record());

    categories << pair;
  }

  // All categories are now loaded.
  QSqlQuery query_feeds(database);
  query_feeds.setForwardOnly(true);

  if (!query_feeds.exec(QSL("SELECT * FROM Feeds;")) || query_feeds.lastError().isValid()) {
    qFatal("Query for obtaining feeds failed. Error message: '%s'.",
           qPrintable(query_feeds.lastError().text()));
  }

  while (query_feeds.next()) {
    // Process this feed.
    StandardFeed::Type type = static_cast<StandardFeed::Type>(query_feeds.value(FDS_DB_TYPE_INDEX).toInt());

    switch (type) {
      case StandardFeed::Atom10:
      case StandardFeed::Rdf:
      case StandardFeed::Rss0X:
      case StandardFeed::Rss2X: {
        FeedAssignmentItem pair;
        pair.first = query_feeds.value(FDS_DB_CATEGORY_INDEX).toInt();
        pair.second = new StandardFeed(query_feeds.record());
        pair.second->setType(type);

        feeds << pair;
        break;
      }

      default:
        break;
    }
  }

  // All data are now obtained, lets create the hierarchy.
  assembleCategories(categories);
  assembleFeeds(feeds);

  // As the last item, add recycle bin, which is needed.
  m_rootItem->appendChild(m_recycleBin);
}

QList<Feed*> FeedsModel::feedsForIndex(const QModelIndex &index) {
  RootItem *item = itemForIndex(index);
  return feedsForItem(item);
}

Feed *FeedsModel::feedForIndex(const QModelIndex &index) {
  RootItem *item = itemForIndex(index);

  if (item->kind() == RootItem::Feeed) {
    return static_cast<Feed*>(item);
  }
  else {
    return NULL;
  }
}

QList<Feed*> FeedsModel::feedsForIndexes(const QModelIndexList &indexes) {
  QList<Feed*> feeds;

  // Get selected feeds for each index.
  foreach (const QModelIndex &index, indexes) {
    feeds.append(feedsForIndex(index));
  }

  // Now we obtained all feeds from corresponding indexes.
  if (indexes.size() != feeds.size()) {
    // Selection contains duplicate feeds (for
    // example situation where feed and its parent category are both
    // selected). So, remove duplicates from the list.
    qSort(feeds.begin(), feeds.end(), RootItem::lessThan);
    feeds.erase(std::unique(feeds.begin(), feeds.end(), RootItem::isEqual), feeds.end());
  }

  return feeds;
}

bool FeedsModel::markFeedsRead(const QList<Feed *> &feeds, int read) {
  QSqlDatabase db_handle = qApp->database()->connection(objectName(), DatabaseFactory::FromSettings);

  if (!db_handle.transaction()) {
    qWarning("Starting transaction for feeds read change.");
    return false;
  }

  QSqlQuery query_read_msg(db_handle);
  query_read_msg.setForwardOnly(true);

  if (!query_read_msg.prepare(QString("UPDATE Messages SET is_read = :read "
                                      "WHERE feed IN (%1) AND is_deleted = 0;").arg(textualFeedIds(feeds).join(QSL(", "))))) {
    qWarning("Query preparation failed for feeds read change.");

    db_handle.rollback();
    return false;
  }

  query_read_msg.bindValue(QSL(":read"), read);

  if (!query_read_msg.exec()) {
    qDebug("Query execution for feeds read change failed.");
    db_handle.rollback();
  }

  // Commit changes.
  if (db_handle.commit()) {
    return true;
  }
  else {
    return db_handle.rollback();
  }
}

bool FeedsModel::markFeedsDeleted(const QList<Feed*> &feeds, int deleted, bool read_only) {
  QSqlDatabase db_handle = qApp->database()->connection(objectName(), DatabaseFactory::FromSettings);

  if (!db_handle.transaction()) {
    qWarning("Starting transaction for feeds clearing.");
    return false;
  }

  QSqlQuery query_delete_msg(db_handle);
  query_delete_msg.setForwardOnly(true);

  if (read_only) {
    if (!query_delete_msg.prepare(QString("UPDATE Messages SET is_deleted = :deleted "
                                          "WHERE feed IN (%1) AND is_deleted = 0 AND is_read = 1;").arg(textualFeedIds(feeds).join(QSL(", "))))) {
      qWarning("Query preparation failed for feeds clearing.");

      db_handle.rollback();
      return false;
    }
  }
  else {
    if (!query_delete_msg.prepare(QString("UPDATE Messages SET is_deleted = :deleted "
                                          "WHERE feed IN (%1) AND is_deleted = 0;").arg(textualFeedIds(feeds).join(QSL(", "))))) {
      qWarning("Query preparation failed for feeds clearing.");

      db_handle.rollback();
      return false;
    }
  }

  query_delete_msg.bindValue(QSL(":deleted"), deleted);

  if (!query_delete_msg.exec()) {
    qDebug("Query execution for feeds clearing failed.");
    db_handle.rollback();
  }

  // Commit changes.
  if (db_handle.commit()) {
    return true;
  }
  else {
    return db_handle.rollback();
  }
}

QHash<int, StandardCategory*> FeedsModel::allCategories() {
  return categoriesForItem(m_rootItem);
}

QHash<int, StandardCategory*> FeedsModel::categoriesForItem(RootItem *root) {
  QHash<int, StandardCategory*> categories;
  QList<RootItem*> parents;

  parents.append(root->childItems());

  while (!parents.isEmpty()) {
    RootItem *item = parents.takeFirst();

    if (item->kind() == RootItem::Cattegory) {
      // This item is category, add it to the output list and
      // scan its children.
      int category_id = item->id();
      StandardCategory *category = item->toCategory();

      if (!categories.contains(category_id)) {
        categories.insert(category_id, category);
      }

      parents.append(category->childItems());
    }
  }

  return categories;
}

QList<Feed*> FeedsModel::allFeeds() {
  return feedsForItem(m_rootItem);
}

QList<Feed*> FeedsModel::feedsForItem(RootItem *root) {
  QList<RootItem*> children = root->getRecursiveChildren();
  QList<Feed*> feeds;

  foreach (RootItem *child, children) {
    if (child->kind() == RootItem::Feeed) {
      feeds.append(child->toFeed());
    }
  }

  return feeds;
}

void FeedsModel::assembleFeeds(FeedAssignment feeds) {
  QHash<int, StandardCategory*> categories = allCategories();

  foreach (const FeedAssignmentItem &feed, feeds) {
    if (feed.first == NO_PARENT_CATEGORY) {
      // This is top-level feed, add it to the root item.
      m_rootItem->appendChild(feed.second);
    }
    else if (categories.contains(feed.first)) {
      // This feed belongs to this category.
      categories.value(feed.first)->appendChild(feed.second);
    }
    else {
      qWarning("Feed '%s' is loose, skipping it.", qPrintable(feed.second->title()));
    }
  }
}

RecycleBin *FeedsModel::recycleBin() const {
  return m_recycleBin;
}

void FeedsModel::assembleCategories(CategoryAssignment categories) {
  QHash<int, RootItem*> assignments;
  assignments.insert(NO_PARENT_CATEGORY, m_rootItem);

  // Add top-level categories.
  while (!categories.isEmpty()) {
    for (int i = 0; i < categories.size(); i++) {
      if (assignments.contains(categories.at(i).first)) {
        // Parent category of this category is already added.
        assignments.value(categories.at(i).first)->appendChild(categories.at(i).second);

        // Now, added category can be parent for another categories, add it.
        assignments.insert(categories.at(i).second->id(),
                           categories.at(i).second);

        // Remove the category from the list, because it was
        // added to the final collection.
        categories.removeAt(i);
        i--;
      }
    }
  }
}
