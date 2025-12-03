#include "lobbies_model.h"

LobbiesModel::LobbiesModel(QObject *parent) : QAbstractListModel(parent) {}

int LobbiesModel::rowCount(const QModelIndex &parent) const {
  if (parent.isValid()) {
    return 0;
  }
  return static_cast<int>(filtered_.size());
}

QVariant LobbiesModel::data(const QModelIndex &index, int role) const {
  if (!index.isValid() || index.row() < 0 ||
      index.row() >= rowCount(index.parent())) {
    return {};
  }

  const auto &entry = filtered_[static_cast<std::size_t>(index.row())];
  switch (role) {
  case LobbyIdRole:
    return entry.lobbyId;
  case NameRole:
    return entry.name;
  case HostNameRole:
    return entry.hostName;
  case HostIdRole:
    return entry.hostId;
  case MemberCountRole:
    return entry.memberCount;
  case PingRole:
    return entry.ping >= 0 ? QVariant(entry.ping) : QVariant();
  default:
    return {};
  }
}

QHash<int, QByteArray> LobbiesModel::roleNames() const {
  return {{LobbyIdRole, "lobbyId"},     {NameRole, "name"},
          {HostNameRole, "hostName"},   {HostIdRole, "hostId"},
          {MemberCountRole, "members"}, {PingRole, "ping"}};
}

void LobbiesModel::setLobbies(std::vector<Entry> list) {
  applyReset(std::move(list));
}

bool LobbiesModel::removeByHostId(const QString &hostId) {
  if (hostId.isEmpty()) {
    return false;
  }
  std::vector<Entry> next = entries_;
  next.erase(std::remove_if(next.begin(), next.end(),
                            [&hostId](const Entry &entry) {
                              return entry.hostId == hostId;
                            }),
             next.end());
  if (next.size() == entries_.size()) {
    return false;
  }
  applyReset(std::move(next));
  return true;
}

bool LobbiesModel::setMemberCount(const QString &lobbyId, int count) {
  if (count < 0) {
    return false;
  }
  bool any = false;
  for (auto &entry : entries_) {
    if (entry.lobbyId == lobbyId && entry.memberCount != count) {
      entry.memberCount = count;
      any = true;
    }
  }
  if (any) {
    filtered_ = filterEntries(entries_);
    if (!filtered_.empty()) {
      emit dataChanged(index(0, 0),
                       index(static_cast<int>(filtered_.size()) - 1, 0),
                       {MemberCountRole});
    }
  }
  return any;
}

bool LobbiesModel::adjustMemberCount(const QString &lobbyId, int delta) {
  if (delta == 0) {
    return false;
  }
  bool any = false;
  for (auto &entry : entries_) {
    if (entry.lobbyId == lobbyId) {
      const int next = std::max(0, entry.memberCount + delta);
      if (next != entry.memberCount) {
        entry.memberCount = next;
        any = true;
      }
      break;
    }
  }
  if (any) {
    filtered_ = filterEntries(entries_);
    if (!filtered_.empty()) {
      emit dataChanged(index(0, 0),
                       index(static_cast<int>(filtered_.size()) - 1, 0),
                       {MemberCountRole});
    }
  }
  return any;
}

void LobbiesModel::setFilter(const QString &text) {
  if (filter_ == text) {
    return;
  }
  filter_ = text;
  filterLower_ = filter_.toLower();
  auto next = filterEntries(entries_);
  const bool sizeChanged = next.size() != filtered_.size();
  filtered_ = std::move(next);
  emit filterChanged();
  if (sizeChanged) {
    beginResetModel();
    endResetModel();
    emit countChanged();
  } else if (!filtered_.empty()) {
    emit dataChanged(index(0, 0),
                     index(static_cast<int>(filtered_.size()) - 1, 0));
  }
}

void LobbiesModel::setSortMode(int mode) {
  if (mode == sortMode_) {
    return;
  }
  if (mode != SortByMembers && mode != SortByName) {
    mode = SortByMembers;
  }
  sortMode_ = mode;
  filtered_ = filterEntries(entries_);
  emit sortModeChanged();
  if (!filtered_.empty()) {
    emit dataChanged(index(0, 0),
                     index(static_cast<int>(filtered_.size()) - 1, 0));
  }
}

std::vector<LobbiesModel::Entry>
LobbiesModel::filterEntries(const std::vector<Entry> &source) const {
  std::vector<Entry> result;
  result.reserve(source.size());
  for (const auto &entry : source) {
    if (matchesFilter(entry)) {
      result.push_back(entry);
    }
  }
  std::stable_sort(result.begin(), result.end(),
                   [this](const Entry &a, const Entry &b) {
                     if (sortMode_ == SortByName) {
                       const int cmp =
                           a.name.toLower().compare(b.name.toLower());
                       if (cmp != 0)
                         return cmp < 0;
                       return a.memberCount > b.memberCount;
                     }
                     if (a.memberCount != b.memberCount) {
                       return a.memberCount > b.memberCount;
                     }
                     return a.name.toLower() < b.name.toLower();
                   });
  return result;
}

bool LobbiesModel::matchesFilter(const Entry &entry) const {
  if (filterLower_.isEmpty()) {
    return true;
  }
  const auto contains = [&](const QString &value) {
    return value.toLower().contains(filterLower_);
  };
  return contains(entry.name) || contains(entry.hostName) ||
         contains(entry.lobbyId);
}

void LobbiesModel::applyReset(std::vector<Entry> list) {
  const bool sizeChanged = list.size() != entries_.size();
  entries_ = std::move(list);
  auto nextFiltered = filterEntries(entries_);
  const bool viewChanged = nextFiltered.size() != filtered_.size();
  filtered_ = std::move(nextFiltered);
  if (sizeChanged || viewChanged) {
    beginResetModel();
    endResetModel();
    emit countChanged();
  } else if (!filtered_.empty()) {
    emit dataChanged(index(0, 0),
                     index(static_cast<int>(filtered_.size()) - 1, 0));
  }
}
