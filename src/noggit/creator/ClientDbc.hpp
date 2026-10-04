#pragma once
#include "Services.hpp"
#include <QByteArray>
#include <QHash>
#include <optional>
namespace Noggit::Creator {
// A WDBC table (the 1.x client's DBFilesClient files): fixed-size records of 32-bit cells and a string block.
// Pure: reads and writes bytes, no client or database access.
class Wdbc {
public:
  static Wdbc parse(QByteArray const& bytes); // throws on anything but a well-formed WDBC file
  QByteArray bytes() const;
  int rows() const { return int(_records.size() / std::max<std::size_t>(1, _columns)); }
  int columns() const { return int(_columns); }
  quint32 cell(int row, int column) const { return _records[std::size_t(row) * _columns + column]; }
  float real(int row, int column) const;
  QString text(int row, int column) const; // the string a cell points at
  int find(quint32 id) const;              // row whose first column is `id`, or -1
  // Replaces (or appends, when the ID is new) the record with this ID; `strings` holds the texts of string cells.
  void put(QVector<quint32> const& record, QHash<int, QString> const& strings);
  void remove(quint32 id);
private:
  quint32 string(QString const& text);
  std::size_t _columns = 0;
  std::vector<quint32> _records;
  QByteArray _strings{1, '\0'};
  QHash<QString, quint32> _offsets;
};
// Spell.dbc of the 1.12 client (173 columns). Creator spells are kept in spell_template, which mirrors these
// columns (verified against every spell of the bundled data), so their client rows are generated from it.
namespace SpellDbc {
constexpr int columns = 173, locales = 8;
// The record for a spell_template row (column names as the world database has them).
QVector<quint32> record(Fields const& spell, QHash<int, QString>& strings);
}
// Readable choices from the client's own lookup tables: spells refer to them by row ID, and both the client
// and the server only know the rows that already exist.
struct ClientChoice { Id id = 0; QString label; double value = 0; };
namespace ClientLists {
QVector<ClientChoice> castTimes(Wdbc const&);  // SpellCastTimes.dbc: "2.5 sec cast"
QVector<ClientChoice> durations(Wdbc const&);  // SpellDuration.dbc: "30 sec"
QVector<ClientChoice> ranges(Wdbc const&);     // SpellRange.dbc: "Medium Range (30 yd)"
QVector<ClientChoice> radii(Wdbc const&);      // SpellRadius.dbc: "10 yd"
QString seconds(double ms);                    // "2.5 sec", "1 min 30 sec"
}
}
